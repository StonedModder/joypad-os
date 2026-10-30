/* SPDX-License-Identifier: GPL-3.0-or-later */
/* NetPad: inject controller state into joypad-os from outside the box.
 *
 * Bring-up transport is the UART (the port already wired to the PC), so the
 * injection path can be exercised before Wi-Fi credentials exist. The wire
 * format is the GhostPad 16-byte frame, the same one the PS5-side service
 * decodes, so one script can drive either transport:
 *
 *   "GPAD" | buttons BE u32 | lx | ly | rx | ry | l2 | r2 | 0 | 0
 *
 * Text command on the UART, one per line:
 *   GPAD <32 hex chars>     inject that frame
 *   NETPAD?                 print what the injector last submitted
 */
#pragma once

#include <stdint.h>

void netpad_start(void);

/* Run one command line. Shared by the UART reader and the TCP transport so the
 * two links accept exactly the same verbs. */
void netpad_handle_command(const char *line);

/* Parse and submit one 16-byte frame. Returns 0 on success. Exposed for tests
 * and for the future Wi-Fi transport, which will call the same path. */
int netpad_submit_frame(const uint8_t frame[16]);

/* The response line produced by the most recent netpad_handle_command() call.
 * Empty when the command produced nothing. Never contains a password. */
const char *netpad_last_reply(void);
