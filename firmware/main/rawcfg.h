/*
 * Raw-frame configuration protocol (EtherType 0x88B5): lets a tool on the
 * retro machine run console commands without any TCP/IP.
 *
 * Frame layout after the 14-byte Ethernet header:
 *   0..2  "WRG"
 *   3     version (1)
 *   4     type: 1 = request, 2 = reply
 *   5..6  sequence number (big endian), copied into the reply
 *   7     part number (reply: 0, 1, 2, ...)
 *   8     flags: bit 0 = last part
 *   9..   text: the command line (request) or output (reply), UTF-8
 *
 * Requests go to the broadcast address; replies go to the requesting MAC.
 */
#pragma once

#include <stdint.h>

/* Called from the Ethernet RX path with a config frame; copies it. */
void rawcfg_input(const uint8_t *frame, uint32_t len);
