/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Wi-Fi transport — see netpad_wifi.h. */

#include "netpad_wifi.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "netpad.h"
#include "netpad/netpad_ota.h"

#if defined(ESP_PLATFORM)
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"

#include "usb/usbd/usbd.h"

#define NVS_NS      "netpad"
#define NVS_SSID    "ssid"
#define NVS_PASS    "pass"
#define TCP_PORT    6970
#define UDP_PORT    6971
#define LINE_MAX    96

static char s_ip[16];
static char s_name[24] = "ghostpad";
static bool s_connected;
static bool s_retry_pending;
static bool s_reconfiguring;   /* set_config is illegal mid-connect */

/* ---- credentials ------------------------------------------------------- */

static size_t nvs_get_str_into(const char *key, char *out, size_t out_len)
{
    nvs_handle_t h;
    out[0] = '\0';
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return (err == ESP_OK) ? len : 0;
}

static void nvs_put_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

static bool load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_get_str_into(NVS_SSID, ssid, ssid_len);
    nvs_get_str_into(NVS_PASS, pass, pass_len);
    return ssid[0] != '\0';
}

/* ---- event handling ---------------------------------------------------- */

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    s_connected = true;
    printf("[netpad] Wi-Fi up, ip=%s — tcp %d, udp %d\n", s_ip, TCP_PORT, UDP_PORT);
    /* The rig is in another room from the PC: expose the OTA uploader once the
     * station has an address so firmware updates need no cable. */
    netpad_ota_start();
}

static void retry_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (!s_reconfiguring) esp_wifi_connect();
    s_retry_pending = false;
    vTaskDelete(NULL);
}

static void on_disconnected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
    s_connected = false;
    s_ip[0] = '\0';
    if (s_retry_pending) return;      /* one pending reconnect is enough */
    s_retry_pending = true;
    /* 15 = 4WAY_HANDSHAKE_TIMEOUT (wrong password), 201 = NO_AP_FOUND (2.4 GHz
     * only on this chip, so a 5 GHz-only SSID looks like this). */
    printf("[netpad] Wi-Fi disconnected (reason=%d), retrying in 2s\n",
           d ? d->reason : -1);
    xTaskCreate(retry_task, "netpad_retry", 2560, NULL, 4, NULL);
}

/* ---- servers ----------------------------------------------------------- */

static void serve_client(int fd)
{
    char line[LINE_MAX];
    size_t len = 0;
    char c;

    for (;;) {
        int n = recv(fd, &c, 1, 0);
        if (n <= 0) break;
        if (c == '\r') continue;
        if (c == '\n') {
            line[len] = '\0';
            if (len > 0) {
                /* Never echo a WIFI line: it carries the password. */
                if (strncmp(line, "GPAD ", 5) == 0) {
                    printf("[netpad] tcp: GPAD frame\n");
                } else if (strncmp(line, "WIFI ", 5) == 0) {
                    printf("[netpad] tcp: WIFI <redacted>\n");
                } else {
                    printf("[netpad] tcp: %s\n", line);
                }
                netpad_handle_command(line);
                const char *rp = netpad_last_reply();
                if (rp && *rp) {
                    char out[200];
                    int rn = snprintf(out, sizeof(out), "%s\n", rp);
                    if (rn > 0) send(fd, out, (size_t)rn, 0);
                }
            }
            len = 0;
            continue;
        }
        if (len < sizeof(line) - 1) line[len++] = c;
    }
    close(fd);
}

static void tcp_task(void *arg)
{
    (void)arg;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(TCP_PORT);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { printf("[netpad] tcp socket failed\n"); vTaskDelete(NULL); return; }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(srv, 2) < 0) {
        printf("[netpad] tcp bind/listen failed on %d\n", TCP_PORT);
        close(srv);
        vTaskDelete(NULL);
        return;
    }
    printf("[netpad] tcp listening on %d\n", TCP_PORT);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd >= 0) {
            /* Greet on connect: over Wi-Fi this is the only proof the link is
             * alive, since the rig's log goes to a UART nobody has plugged in. */
            char hello[96];
            int hn = snprintf(hello, sizeof(hello), "OK GHOSTPAD %s mode=%s\n",
                              s_name, usbd_get_mode_name(usbd_get_mode()));
            if (hn > 0) send(fd, hello, (size_t)hn, 0);
            serve_client(fd);
        }
    }
}

static void udp_task(void *arg)
{
    (void)arg;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(UDP_PORT);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { vTaskDelete(NULL); return; }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("[netpad] udp bind failed on %d\n", UDP_PORT);
        close(fd);
        vTaskDelete(NULL);
        return;
    }

    char buf[64];
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &flen);
        if (n <= 0) continue;
        buf[n] = '\0';
        if (strncmp(buf, "GHOSTPAD?", 9) != 0) continue;

        char reply[160];
        char status[64];
        netpad_wifi_status(status, sizeof(status));
        snprintf(reply, sizeof(reply),
                 "{\"name\":\"%s\",\"mode\":\"%s\",\"ip\":\"%s\",\"state\":\"%s\"}\n",
                 s_name, usbd_get_mode_name(usbd_get_mode()), s_ip, status);
        sendto(fd, reply, (int)strlen(reply), 0, (struct sockaddr *)&from, flen);
    }
}

/* ---- outbound sink ----------------------------------------------------- */

/* The console side (Ghostcontrol's gc_ghostpad.c) is a TCP listener on 6967
 * that eats 16-byte GPAD packets and releases the pad if none arrive for
 * 250 ms. Streaming to it avoids the whole USB authentication problem: the
 * console never has to accept us as a controller. */
static char s_sink_host[48];
static uint16_t s_sink_port = 6967;
static bool s_sink_enabled;
static SemaphoreHandle_t s_sink_lock;
static uint8_t s_sink_frame[16];
static uint32_t s_sink_sent;
static uint32_t s_sink_errors;
static int s_sink_fd = -1;

void netpad_wifi_sink_publish(const uint8_t frame[16])
{
    if (!s_sink_enabled || !s_sink_lock || !frame) return;
    if (xSemaphoreTake(s_sink_lock, pdMS_TO_TICKS(20)) != pdTRUE) return;
    memcpy(s_sink_frame, frame, 16);
    xSemaphoreGive(s_sink_lock);
}

bool netpad_wifi_set_sink(const char *arg)
{
    if (!arg || !*arg) return false;

    if (!strcasecmp(arg, "off")) {
        s_sink_enabled = false;
        if (s_sink_fd >= 0) {
            /* DISC is a full 16-byte packet, not a bare 4-byte token: the
             * console reads fixed-size packets, so a short write desyncs it. */
            uint8_t disc[16] = {0};
            memcpy(disc, "DISC", 4);
            send(s_sink_fd, disc, 16, 0);
            close(s_sink_fd);
            s_sink_fd = -1;
        }
        s_sink_host[0] = '\0';
        printf("[netpad] sink disabled\n");
        return true;
    }

    char host[48];
    uint16_t port = 6967;
    const char *colon = strrchr(arg, ':');
    if (colon) {
        size_t n = (size_t)(colon - arg);
        if (n == 0 || n >= sizeof(host)) return false;
        memcpy(host, arg, n);
        host[n] = '\0';
        int p = atoi(colon + 1);
        if (p <= 0 || p > 65535) return false;
        port = (uint16_t)p;
    } else {
        snprintf(host, sizeof(host), "%s", arg);
    }

    snprintf(s_sink_host, sizeof(s_sink_host), "%s", host);
    s_sink_port = port;
    s_sink_enabled = true;
    s_sink_sent = 0;
    s_sink_errors = 0;

    /* Never emit a packet the service would reject: start from a neutral frame
     * (magic + centred sticks) instead of the zeroed mailbox. */
    if (s_sink_lock && xSemaphoreTake(s_sink_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        memset(s_sink_frame, 0, sizeof(s_sink_frame));
        s_sink_frame[0] = 'G'; s_sink_frame[1] = 'P';
        s_sink_frame[2] = 'A'; s_sink_frame[3] = 'D';
        s_sink_frame[8] = s_sink_frame[9] = 128;
        s_sink_frame[10] = s_sink_frame[11] = 128;
        xSemaphoreGive(s_sink_lock);
    }

    printf("[netpad] sink -> %s:%u\n", s_sink_host, s_sink_port);
    return true;
}

void netpad_wifi_sink_status(char *out, unsigned out_len)
{
    if (!out || out_len == 0) return;
    if (!s_sink_enabled) {
        snprintf(out, out_len, "SINK off");
        return;
    }
    snprintf(out, out_len, "SINK %s:%u %s sent=%lu errors=%lu",
             s_sink_host, s_sink_port, s_sink_fd >= 0 ? "connected" : "connecting",
             (unsigned long)s_sink_sent, (unsigned long)s_sink_errors);
}

static void sink_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_sink_enabled) {
            if (s_sink_fd >= 0) { close(s_sink_fd); s_sink_fd = -1; }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (s_sink_fd < 0) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd >= 0) {
                struct sockaddr_in a = {0};
                a.sin_family = AF_INET;
                a.sin_port = htons(s_sink_port);
                a.sin_addr.s_addr = inet_addr(s_sink_host);
                if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) {
                    s_sink_fd = fd;
                    printf("[netpad] sink connected %s:%u\n", s_sink_host, s_sink_port);
                } else {
                    close(fd);
                }
            }
            if (s_sink_fd < 0) {
                s_sink_errors++;
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }

        uint8_t frame[16];
        if (s_sink_lock && xSemaphoreTake(s_sink_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            memcpy(frame, s_sink_frame, 16);
            xSemaphoreGive(s_sink_lock);
        } else {
            memset(frame, 0, sizeof(frame));
            frame[0] = 'G'; frame[1] = 'P'; frame[2] = 'A'; frame[3] = 'D';
            frame[8] = frame[9] = frame[10] = frame[11] = 128;
        }

        /* 20 Hz: well inside the service's 250 ms stale window, and cheap. */
        if (send(s_sink_fd, frame, 16, 0) != 16) {
            s_sink_errors++;
            close(s_sink_fd);
            s_sink_fd = -1;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        s_sink_sent++;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---- public ------------------------------------------------------------ */

static bool s_wifi_ready;

/* Boot the Wi-Fi stack exactly once — even with no credentials stored.
 * Without this, a later WIFI command would configure an uninitialised driver
 * (esp_wifi_set_config returns ESP_ERR_WIFI_NOT_INIT) and silently do nothing. */
static void wifi_stack_init(void)
{
    if (s_wifi_ready) return;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("[netpad] esp_netif_init: %d\n", err);
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("[netpad] event loop: %d\n", err);
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("[netpad] esp_wifi_init: %d\n", err);
    }
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                        on_disconnected, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        on_got_ip, NULL, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("[netpad] esp_wifi_start: %d\n", err);
    }
    s_wifi_ready = true;
    printf("[netpad] Wi-Fi stack ready\n");
}

bool netpad_wifi_set(const char *ssid, const char *pass)
{
    if (!ssid || !*ssid) return false;
    wifi_stack_init();

    char stored_pass[64] = {0};
    nvs_get_str_into(NVS_PASS, stored_pass, sizeof(stored_pass));
    if (!pass || !*pass) pass = stored_pass;   /* empty keeps the stored one */

    nvs_put_str(NVS_SSID, ssid);
    if (pass && *pass) nvs_put_str(NVS_PASS, pass);

    wifi_config_t cfg = {0};
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", ssid);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", pass ? pass : "");

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        printf("[netpad] disconnect before reconfigure: %d\n", err);
    }

    /* The driver refuses set_config() while a connect attempt is in flight
     * ("sta is connecting, cannot set config" / ESP_ERR_WIFI_STATE), and
     * disconnect is asynchronous — so retry until it settles. */
    s_reconfiguring = true;
    esp_err_t cfg_err = ESP_FAIL;
    for (int i = 0; i < 60; i++) {
        cfg_err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        if (cfg_err == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_reconfiguring = false;
    if (cfg_err != ESP_OK) {
        printf("[netpad] set_config failed: %d (ssid '%s')\n", cfg_err, ssid);
        return false;
    }

    err = esp_wifi_connect();
    printf("[netpad] joining '%s' (%s) connect=%d\n", ssid,
           (pass && *pass) ? "with password" : "open", err);
    return err == ESP_OK;
}

void netpad_wifi_status(char *out, unsigned out_len)
{
    snprintf(out, out_len, "%s%s%s",
             s_connected ? "connected" : "joining",
             s_connected ? " " : "",
             s_connected ? s_ip : "");
}

void netpad_wifi_start(void)
{
    char ssid[33] = {0};
    char pass[64] = {0};

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_name, sizeof(s_name), "ghostpad-%02x%02x", mac[4], mac[5]);
    }

    wifi_stack_init();                 /* even when no credentials are stored */

    xTaskCreate(tcp_task, "netpad_tcp", 4096, NULL, 4, NULL);
    xTaskCreate(udp_task, "netpad_udp", 4096, NULL, 3, NULL);
    s_sink_lock = xSemaphoreCreateMutex();
    xTaskCreate(sink_task, "netpad_sink", 4096, NULL, 3, NULL);

    if (!load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        printf("[netpad] no Wi-Fi credentials yet — send: WIFI <ssid> <password>\n");
        return;
    }

    netpad_wifi_set(ssid, pass);
}

#else

void netpad_wifi_start(void) {}
bool netpad_wifi_set(const char *ssid, const char *pass) { (void)ssid; (void)pass; return false; }
void netpad_wifi_status(char *out, unsigned out_len) { snprintf(out, out_len, "unavailable"); }

#endif
