/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Wi-Fi transport for the NetPad injector.
 *
 * The rig joins an existing network as a station (the PC that drives it is
 * wired, so a SoftAP would be unreachable). Once it has an address it serves:
 *
 *   TCP 6970  line protocol — the same commands as the UART: GPAD <hex>,
 *             NETPAD?, MODE <name>, MODE?  (so one script drives either link)
 *   UDP 6971  discovery — send "GHOSTPAD?" and get one JSON line back
 *
 * Credentials are stored in NVS under "netpad" and are never printed.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Start the transport: joins the stored network if credentials exist. */
void netpad_wifi_start(void);

/* Store credentials and (re)connect. Empty password keeps the stored one. */
bool netpad_wifi_set(const char *ssid, const char *pass);

/* One-line status for the WIFI? command. */
void netpad_wifi_status(char *out, unsigned out_len);
