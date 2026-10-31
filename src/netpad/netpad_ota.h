/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * netpad_ota: cable-free firmware updates for the rig.
 *
 * The rig sits in another room from the PC, so a UART flash is not available
 * during normal work. This serves POST /ota on port 80 and writes the body to
 * the inactive OTA slot, then reboots into it. The bootloader's rollback stays
 * armed until the new image confirms itself, so a bad upload returns to the
 * previous slot instead of bricking the rig.
 */
#pragma once

/* Start the upload server. Safe to call more than once (idempotent); call it
 * once the station has an IP so the server is reachable. */
void netpad_ota_start(void);
