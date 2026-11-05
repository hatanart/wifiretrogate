/*
 * Service address (default 192.0.2.1): WifiRetroGate's own services (Telnet)
 * as seen from the retro machine.
 *
 * The address is outside the LAN, so the client sends these packets to its
 * gateway. The bridge picks them out by destination IP before forwarding and
 * hands them to a small lwIP interface. Replies go back to the client with the
 * gateway's MAC as the sender, so they look like any reply via the gateway.
 * No MAC of our own and no ARP are involved; the LAN cannot reach it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Add the lwIP interface (bridge mode; needs esp_netif_init()) */
void svcnet_start(const uint8_t client_mac[6]);

/* True if this Ethernet frame from the client is an IPv4 packet for the
 * service address; the frame is then consumed (copied). */
bool svcnet_input(const uint8_t *frame, uint32_t len);
