/*
 * Wi-Fi station manager for bridge mode.
 *
 * The station uses the client's MAC. It scans for saved networks, connects to
 * the strongest one in range, tries the others if that fails, and never gives up.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_wifi.h"
#include "esp_private/wifi.h"

typedef enum {
	WIFI_STA_SEARCHING,	/* scanning */
	WIFI_STA_NOT_FOUND,	/* no saved network in range */
	WIFI_STA_CONNECTING,
	WIFI_STA_FAILED,	/* last attempt failed, see reason */
	WIFI_STA_CONNECTED,
} wifi_sta_state_t;

typedef struct {
	wifi_sta_state_t state;
	char ssid[33];		/* network being used or tried */
	uint8_t reason;		/* last disconnect reason (WIFI_REASON_...) */
	int rssi;
	uint8_t channel;
} wifi_sta_status_t;

/* Fast check for the forwarding path */
extern volatile bool wifi_sta_up;

/* Start the station with the given MAC; frames received while connected go to
 * rx. connected (may be NULL) is called from the Wi-Fi task after every
 * successful connection. */
void wifi_sta_start(const uint8_t mac[6], wifi_rxcb_t rx, void (*connected)(void));

/* True when every saved network has been tried at least once since start-up
 * and none could be reached (false again forever after the first connection) */
bool wifi_sta_unreachable_since_boot(void);

void wifi_sta_get_status(wifi_sta_status_t *status);

/* Scan for networks, strongest first. Safe to call from any task while Wi-Fi
 * is started (station or setup mode); concurrent scans wait for each other. */
esp_err_t wifi_scan(wifi_ap_record_t *records, uint16_t *count);

/* Drop the connection; the manager searches and connects again */
void wifi_sta_reconnect(void);

/* Short text for a disconnect reason (max 15 characters) */
const char *wifi_sta_reason_text(uint8_t reason);
