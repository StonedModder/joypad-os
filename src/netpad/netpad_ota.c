/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netpad/netpad_ota.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static httpd_handle_t s_server;

static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "no update partition");
        return ESP_FAIL;
    }
    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "ota_begin failed");
        return ESP_FAIL;
    }

    char buf[2048];
    int remaining = req->content_len;
    while (remaining > 0) {
        int want = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int got = httpd_req_recv(req, buf, want);
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT)
                continue;
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        if (esp_ota_write(handle, buf, got) != ESP_OK) {
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "ota_write failed");
            return ESP_FAIL;
        }
        remaining -= got;
    }

    if (esp_ota_end(handle) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "ota_end failed (image rejected)");
        return ESP_FAIL;
    }
    if (esp_ota_set_boot_partition(update) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "set_boot_partition failed");
        return ESP_FAIL;
    }

    printf("[netpad] ota: image accepted (%d bytes) -> %s, rebooting\n",
           req->content_len, update->label);
    httpd_resp_sendstr(req, "ota accepted, rebooting\n");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t status_get(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    char body[192];
    snprintf(body, sizeof(body),
             "{\"app\":\"netpad\",\"running\":\"%s\",\"ota\":true}\n",
             running ? running->label : "?");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

void netpad_ota_start(void)
{
    if (s_server)
        return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;
    cfg.max_open_sockets = 3;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        s_server = NULL;
        printf("[netpad] ota: http server failed to start\n");
        return;
    }
    httpd_uri_t post = {.uri = "/ota", .method = HTTP_POST, .handler = ota_post};
    httpd_uri_t status = {.uri = "/status", .method = HTTP_GET, .handler = status_get};
    httpd_register_uri_handler(s_server, &post);
    httpd_register_uri_handler(s_server, &status);
    printf("[netpad] ota: ready on :80 (POST /ota, GET /status)\n");
}
