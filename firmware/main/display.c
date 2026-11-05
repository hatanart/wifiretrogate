/*
 * Status screens on the OLED, with burn-in protection. See display.h.
 */
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "bridge.h"
#include "display.h"
#include "oled.h"
#include "status.h"
#include "wifi_sta.h"

#if CONFIG_WRG_OLED_ENABLE

#ifdef CONFIG_WRG_OLED_SH1106
#define OLED_TYPE	OLED_SH1106
#else
#define OLED_TYPE	OLED_SSD1306
#endif
#ifdef CONFIG_WRG_OLED_ROTATE
#define OLED_ROTATE	true
#else
#define OLED_ROTATE	false
#endif

static uint32_t s_last_state = UINT32_MAX;
static int64_t s_last_change_us;
static bool s_screen_on = true;

void display_init(void)
{
	oled_config_t config = {
		.sda_gpio = CONFIG_WRG_OLED_SDA,
		.scl_gpio = CONFIG_WRG_OLED_SCL,
		.address = CONFIG_WRG_OLED_ADDRESS,
		.type = OLED_TYPE,
		.height = CONFIG_WRG_OLED_HEIGHT,
		.rotate180 = OLED_ROTATE,
		.contrast = CONFIG_WRG_OLED_CONTRAST,
	};
	oled_init(&config);	/* runs without a display too */
	s_last_change_us = esp_timer_get_time();
}

bool display_present(void)
{
	return oled_present();
}

void display_wake(void)
{
	s_last_change_us = esp_timer_get_time();
	if (!s_screen_on) {
		oled_power(true);
		s_screen_on = true;
	}
}

/* Burn-in protection: any state change (not counters) wakes the screen; the
 * screen goes off after a while without changes; the text moves by a pixel
 * every minute. Sends the screen if it is on. */
static void refresh(uint32_t state)
{
	int64_t now = esp_timer_get_time();

	if (state != s_last_state) {
		s_last_state = state;
		display_wake();
	} else if (CONFIG_WRG_OLED_SLEEP > 0 && s_screen_on
		   && now - s_last_change_us > CONFIG_WRG_OLED_SLEEP * 1000000LL) {
		oled_power(false);
		s_screen_on = false;
	}

	static const int shifts[] = { 0, 1, 2, 1 };
	oled_set_shift(shifts[(now / 60000000) % 4]);

	if (s_screen_on) {
		oled_update();
	}
}

/* The first row answers "does it work, and if not, why?" */
void display_bridge_status(void)
{
	bridge_status_t bridge;
	wifi_sta_status_t wifi;
	bridge_get_status(&bridge);
	wifi_sta_get_status(&wifi);

	char line[32];
	status_line(line, sizeof line);
	oled_print(0, line);

	if (bridge.eth_up) {
		oled_printf(2, "ETH  LINK %s %s", bridge.eth_100m ? "100M" : "10M",
			    bridge.eth_full_duplex ? "full" : "half");
	} else {
		oled_print(2, "ETH  no link");
	}
	if (bridge.client_known) {
		const uint8_t *m = bridge.client_mac;
		oled_printf(3, "MAC  %02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
	} else {
		oled_print(3, "MAC  -");
	}
	oled_printf(4, "WIFI %s", wifi.ssid[0] ? wifi.ssid : "-");
	/* hints: phone hotspots often run on 5 GHz, which the ESP32 cannot see;
	 * Ethernet II IPX from the client reaches only other WifiRetroGates */
	if (wifi.state == WIFI_STA_NOT_FOUND) {
		oled_print(5, "hotspot? use 2.4 GHz");
	} else if (bridge.ipx_ethernet_ii) {
		oled_print(5, "IPX: use 802.2 frame");
	} else {
		oled_print(5, "");
	}
	oled_printf(6, "TX %-7lu RX %lu", (unsigned long) bridge.eth_to_wifi,
		    (unsigned long) bridge.wifi_to_eth);
	oled_printf(7, "DROP %-6lu ERR %lu",
		    (unsigned long) (bridge.drop_other_mac + bridge.drop_no_wifi + bridge.drop_no_eth),
		    (unsigned long) (bridge.err_wifi_tx + bridge.err_eth_tx));

	uint32_t state = bridge.client_known | bridge.eth_up << 1 | bridge.ipx_ethernet_ii << 5
			 | wifi.state << 2 | wifi.reason << 8;
	refresh(state);
}

void display_setup(const char *ap_ssid, const char *ap_password, const char *message)
{
	oled_print(0, "SETUP MODE");
	oled_print(1, "");
	oled_print(2, "Join Wi-Fi:");
	oled_print(3, ap_ssid);
	oled_printf(4, ap_password[0] ? "PASS %s" : "(open network)", ap_password);
	oled_print(5, "");
	oled_print(6, "http://192.168.4.1");
	oled_print(7, message);

	/* the message changes when something happens in the web page */
	uint32_t state = 0x80000000;
	for (const char *p = message; *p; p++) {
		state = state * 31 + (uint8_t) *p;
	}
	refresh(state);
}

#else

void display_init(void) {}
bool display_present(void) { return false; }
void display_bridge_status(void) {}
void display_setup(const char *ap_ssid, const char *ap_password, const char *message) {}
void display_wake(void) {}

#endif
