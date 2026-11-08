/*
 * Ethernet side and frame forwarding.
 *
 * One wired client. The Wi-Fi station takes over the client's MAC address,
 * and frames are forwarded unchanged in both directions.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
	bool client_known;
	uint8_t client_mac[6];
	bool eth_up;
	bool eth_100m;
	bool eth_full_duplex;

	uint32_t eth_to_wifi, wifi_to_eth;
	uint32_t drop_other_mac;	/* Ethernet frames from machines other than the client */
	uint32_t drop_echo;		/* client's own broadcasts repeated by the AP */
	uint32_t drop_no_wifi, drop_no_eth;
	uint32_t err_wifi_tx, err_eth_tx;	/* other side's buffers full */
	uint32_t fix_tx_8021h, fix_rx_8021h;	/* IPX/AARP Ethernet II frames given the 802.1H header in software */
	uint32_t fix_rx_length;		/* 802.3 length fields repaired (ESP32 delivers 0) */
	bool ipx_ethernet_ii;		/* client sent Ethernet II IPX within the last minute */
	bool asks_known;		/* client sent an ARP request within the last 10 minutes */
	uint32_t asks_ip;		/* ...for this address (network byte order) */
	uint32_t asks_age_s;
	bool dhcp_no_answer;		/* client asks for DHCP, no reply for 8 s or more */
	uint32_t dhcp_waiting_s;
	bool gateway_stand_in;		/* no Wi-Fi since start: we answer the client's ARP */
	int stand_in_count;		/* addresses answered for (gateway, ...) */
	uint32_t stand_in_ip;		/* the last one (network byte order) */
	uint8_t last_other_mac[6];
} bridge_status_t;

/* Start Ethernet, learn the client MAC (blocks until known), then start Wi-Fi. */
void bridge_start(void);

/* Setup mode: nothing is bridged. Ethernet answers the raw-frame config tool
 * and has the service address with a DHCP server, for Telnet. */
void bridge_start_setup(void);

void bridge_get_status(bridge_status_t *status);

/* Other machines seen on the Ethernet side (not the client) */
#define BRIDGE_MAX_OTHER_MACS	8
typedef struct {
	uint8_t mac[6];
	uint32_t frames;
} bridge_other_mac_t;
int bridge_get_other_macs(bridge_other_mac_t *macs, int max);

/* The client's IPv4 addresses (network byte order), newest first: the newest
 * always, others if seen within the last 10 minutes. Learned from the
 * sender address of its ARP packets, or from the source address of its IPv4
 * packets until the first ARP. Returns the count (0 = none seen yet). */
#define BRIDGE_MAX_CLIENT_IPS	4
int bridge_get_client_ips(uint32_t *ips, int max);

/* Send a frame of our own on the Ethernet side (config protocol replies) */
esp_err_t bridge_eth_send(const void *frame, size_t len);
void bridge_eth_mac(uint8_t mac[6]);

/* EtherType of the raw-frame configuration protocol (IEEE local experimental) */
#define WRG_ETHERTYPE	0x88b5
