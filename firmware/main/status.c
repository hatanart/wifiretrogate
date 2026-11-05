/*
 * The status line. See status.h.
 */
#include <stdio.h>
#include "bridge.h"
#include "status.h"
#include "wifi_sta.h"

void status_line(char *text, size_t size)
{
	bridge_status_t bridge;
	wifi_sta_status_t wifi;
	bridge_get_status(&bridge);
	wifi_sta_get_status(&wifi);

	if (!bridge.client_known) {
		snprintf(text, size, "WAITING FOR CLIENT");
	} else if (!bridge.eth_up) {
		snprintf(text, size, "ETH: no cable");
	} else {
		switch (wifi.state) {
		case WIFI_STA_CONNECTED:
			snprintf(text, size, "OK %-11.11s%4ddBm", wifi.ssid, wifi.rssi);
			break;
		case WIFI_STA_SEARCHING:
			snprintf(text, size, "WIFI: searching...");
			break;
		case WIFI_STA_NOT_FOUND:
			snprintf(text, size, "WIFI: no known net");
			break;
		case WIFI_STA_CONNECTING:
			snprintf(text, size, "WIFI: connecting...");
			break;
		default:
			snprintf(text, size, "WIFI: %s", wifi_sta_reason_text(wifi.reason));
			break;
		}
	}
}
