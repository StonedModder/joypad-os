/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Wi-Fi transport — see netpad_wifi.h. */

#include "netpad_wifi.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "netpad.h"

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
}

static void on_disconnected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    s_connected = false;
    s_ip[0] = '\0';
    printf("[netpad] Wi-Fi disconnected, retrying\n");
    esp_wifi_connect();
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
                printf("[netpad] tcp: %s\n", strncmp(line, "GPAD ", 5) == 0 ? "GPAD frame" : line);
                netpad_handle_command(line);
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
        if (fd >= 0) serve_client(fd);
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

/* ---- public ------------------------------------------------------------ */

bool netpad_wifi_set(const char *ssid, const char *pass)
{
    if (!ssid || !*ssid) return false;

    char stored_pass[64] = {0};
    nvs_get_str_into(NVS_PASS, stored_pass, sizeof(stored_pass));
    if (!pass || !*pass) pass = stored_pass;   /* empty keeps the stored one */

    nvs_put_str(NVS_SSID, ssid);
    if (pass && *pass) nvs_put_str(NVS_PASS, pass);

    wifi_config_t cfg = {0};
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", ssid);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", pass ? pass : "");
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
    printf("[netpad] joining '%s' (%s)\n", ssid, (pass && *pass) ? "with password" : "open");
    return true;
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

    if (!load_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        printf("[netpad] no Wi-Fi credentials yet — send: WIFI <ssid> <password>\n");
        return;
    }

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&init_cfg);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                        on_disconnected, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        on_got_ip, NULL, NULL);

    wifi_config_t cfg = {0};
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", ssid);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", pass);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_start();
    esp_wifi_connect();
    printf("[netpad] joining '%s' (password stored: %s)\n", ssid, pass[0] ? "yes" : "no");

    xTaskCreate(tcp_task, "netpad_tcp", 4096, NULL, 4, NULL);
    xTaskCreate(udp_task, "netpad_udp", 4096, NULL, 3, NULL);
}

#else

void netpad_wifi_start(void) {}
bool netpad_wifi_set(const char *ssid, const char *pass) { (void)ssid; (void)pass; return false; }
void netpad_wifi_status(char *out, unsigned out_len) { snprintf(out, out_len, "unavailable"); }

#endif
