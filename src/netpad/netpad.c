/* SPDX-License-Identifier: GPL-3.0-or-later */
/* NetPad injector — see netpad.h for the wire format and the UART commands. */

#include "netpad.h"

#include <stdio.h>
#include <string.h>

#include "core/buttons.h"
#include "core/input_event.h"
#include "core/router/router.h"

#if defined(ESP_PLATFORM)
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

/* Console button bits, mirroring the PS5-side decoder (gc_ghostpad.c). */
#define GP_BTN_L3       0x00000001u
#define GP_BTN_R3       0x00000002u
#define GP_BTN_OPTIONS  0x00000008u
#define GP_BTN_UP       0x00000010u
#define GP_BTN_RIGHT    0x00000020u
#define GP_BTN_DOWN     0x00000040u
#define GP_BTN_LEFT     0x00000080u
#define GP_BTN_L2       0x00000100u
#define GP_BTN_R2       0x00000200u
#define GP_BTN_L1       0x00000400u
#define GP_BTN_R1       0x00000800u
#define GP_BTN_TRIANGLE 0x00001000u
#define GP_BTN_CIRCLE   0x00002000u
#define GP_BTN_CROSS    0x00004000u
#define GP_BTN_SQUARE   0x00008000u
#define GP_BTN_CREATE   0x00010000u
#define GP_BTN_TOUCHPAD 0x00100000u

/* Last state submitted, so "NETPAD?" can report it without touching the router. */
static uint32_t s_last_buttons;
static uint8_t s_last_analog[7];
static uint32_t s_frames;

static uint32_t map_buttons(uint32_t gp)
{
    uint32_t b = 0;
    if (gp & GP_BTN_CROSS)    b |= JP_BUTTON_B1;
    if (gp & GP_BTN_CIRCLE)   b |= JP_BUTTON_B2;
    if (gp & GP_BTN_SQUARE)   b |= JP_BUTTON_B3;
    if (gp & GP_BTN_TRIANGLE) b |= JP_BUTTON_B4;
    if (gp & GP_BTN_L1)       b |= JP_BUTTON_L1;
    if (gp & GP_BTN_R1)       b |= JP_BUTTON_R1;
    if (gp & GP_BTN_L2)       b |= JP_BUTTON_L2;
    if (gp & GP_BTN_R2)       b |= JP_BUTTON_R2;
    if (gp & GP_BTN_CREATE)   b |= JP_BUTTON_S1;
    if (gp & GP_BTN_OPTIONS)  b |= JP_BUTTON_S2;
    if (gp & GP_BTN_L3)       b |= JP_BUTTON_L3;
    if (gp & GP_BTN_R3)       b |= JP_BUTTON_R3;
    if (gp & GP_BTN_UP)       b |= JP_BUTTON_DU;
    if (gp & GP_BTN_DOWN)     b |= JP_BUTTON_DD;
    if (gp & GP_BTN_LEFT)     b |= JP_BUTTON_DL;
    if (gp & GP_BTN_RIGHT)    b |= JP_BUTTON_DR;
    if (gp & GP_BTN_TOUCHPAD) b |= JP_BUTTON_A2;
    return b;
}

int netpad_submit_frame(const uint8_t frame[16])
{
    if (!frame || frame[0] != 'G' || frame[1] != 'P' || frame[2] != 'A' || frame[3] != 'D') {
        return -1;
    }

    const uint32_t gp_buttons = ((uint32_t)frame[4] << 24) | ((uint32_t)frame[5] << 16) |
                                ((uint32_t)frame[6] << 8) | (uint32_t)frame[7];

    input_event_t ev = {0};
    ev.dev_addr = 0xEE;                    /* synthetic address, never a real device */
    ev.instance = 0;
    ev.type = INPUT_TYPE_GAMEPAD;
    ev.transport = INPUT_TRANSPORT_UART;
    ev.layout = LAYOUT_MODERN_4FACE;
    ev.buttons = map_buttons(gp_buttons);
    ev.analog[0] = frame[8];               /* LX */
    ev.analog[1] = frame[9];               /* LY */
    ev.analog[2] = frame[10];              /* RX */
    ev.analog[3] = frame[11];              /* RY */
    ev.analog[4] = frame[12];              /* L2 */
    ev.analog[5] = frame[13];              /* R2 */
    ev.valid_fields = 0;                   /* full state: every field is owned */

    s_last_buttons = ev.buttons;
    memcpy(s_last_analog, ev.analog, sizeof(s_last_analog));
    s_frames++;

    router_submit_input(&ev);
    return 0;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "GPAD <32 hex>" -> submit */
static void handle_line(char *line)
{
    if (strncmp(line, "NETPAD?", 7) == 0) {
        printf("[netpad] frames=%lu buttons=0x%08lX analog=%u,%u,%u,%u,%u,%u\n",
               (unsigned long)s_frames, (unsigned long)s_last_buttons,
               s_last_analog[0], s_last_analog[1], s_last_analog[2],
               s_last_analog[3], s_last_analog[4], s_last_analog[5]);
        return;
    }

    if (strncmp(line, "GPAD ", 5) != 0) {
        return;
    }

    const char *hex = line + 5;
    uint8_t frame[16];
    for (int i = 0; i < 16; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            printf("[netpad] bad hex at byte %d\n", i);
            return;
        }
        frame[i] = (uint8_t)((hi << 4) | lo);
    }

    if (netpad_submit_frame(frame) == 0) {
        printf("[netpad] frame %lu buttons=0x%08lX\n",
               (unsigned long)s_frames, (unsigned long)s_last_buttons);
    } else {
        printf("[netpad] rejected frame (bad magic)\n");
    }
}

#if defined(ESP_PLATFORM)
static void netpad_uart_task(void *arg)
{
    (void)arg;
    char line[80];
    size_t len = 0;
    uint8_t buf[64];

    for (;;) {
        int n = uart_read_bytes(UART_NUM_0, buf, sizeof(buf), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[len] = '\0';
                if (len > 0) handle_line(line);
                len = 0;
                continue;
            }
            if (len < sizeof(line) - 1) line[len++] = c;
        }
    }
}
#endif

void netpad_start(void)
{
#if defined(ESP_PLATFORM)
    /* RX only: the console keeps UART0 for log output, so no TX buffer. */
    esp_err_t err = uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("[netpad] uart_driver_install failed: %d\n", err);
        return;
    }
    xTaskCreate(netpad_uart_task, "netpad_uart", 4096, NULL, 5, NULL);
    printf("[netpad] injection ready on UART0 — send: GPAD <32 hex chars>\n");
#else
    printf("[netpad] no transport on this platform\n");
#endif
}
