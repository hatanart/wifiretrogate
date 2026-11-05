/*
 * WifiRetroGate for the WT32-ETH01.
 *
 * Two modes, chosen at boot:
 *   bridge mode  - transparent Ethernet <-> Wi-Fi bridge (normal operation)
 *   setup mode   - access point with a web page for Wi-Fi networks; entered
 *                  when no network is saved, or by a long button press
 * Switching modes restarts the device.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "bridge.h"
#include "button.h"
#include "display.h"
#include "eventlog.h"
#include "serial_console.h"
#include "settings.h"
#include "setup.h"
#include "telnet.h"
#include "wifi_sta.h"
#include "wrg.h"

static const char *TAG = "wrg";

/* survives esp_restart(): asks the next boot to enter setup mode */
#define SETUP_REQUEST	0x5e7a9b01
static RTC_NOINIT_ATTR uint32_t s_setup_request;

static bool s_setup_mode;

bool wrg_setup_mode(void)
{
	return s_setup_mode;
}

void wrg_restart(bool setup_mode)
{
	s_setup_request = setup_mode ? SETUP_REQUEST : 0;

	/* leave the Wi-Fi network properly: the deauthentication frame makes the
	 * AP drop the association now instead of after its inactivity timeout */
	if (esp_wifi_disconnect() == ESP_OK) {
		vTaskDelay(pdMS_TO_TICKS(200));
	}
	esp_restart();
}

static void long_press(void)
{
	/* bridge mode: go to setup; setup mode: leave it */
	wrg_restart(!s_setup_mode);
}

static void print_status(void)
{
	bridge_status_t b;
	wifi_sta_status_t w;
	bridge_get_status(&b);
	wifi_sta_get_status(&w);

	ESP_LOGI(TAG, "%s | ETH %s | WIFI \"%s\" state %d %d dBm | client %02x:%02x:%02x:%02x:%02x:%02x",
		 b.eth_up && w.state == WIFI_STA_CONNECTED ? "OK" : "--",
		 b.eth_up ? "link" : "down", w.ssid, w.state, w.rssi,
		 b.client_mac[0], b.client_mac[1], b.client_mac[2],
		 b.client_mac[3], b.client_mac[4], b.client_mac[5]);
	ESP_LOGI(TAG, "  eth->wifi %lu  wifi->eth %lu | drop: other-mac %lu echo %lu no-wifi %lu no-eth %lu | tx-err wifi %lu eth %lu",
		 (unsigned long) b.eth_to_wifi, (unsigned long) b.wifi_to_eth,
		 (unsigned long) b.drop_other_mac, (unsigned long) b.drop_echo,
		 (unsigned long) b.drop_no_wifi, (unsigned long) b.drop_no_eth,
		 (unsigned long) b.err_wifi_tx, (unsigned long) b.err_eth_tx);
}

static void status_task(void *arg)
{
	for (unsigned seconds = 1; ; seconds++) {
		display_bridge_status();
		if (seconds % 5 == 0) {
			print_status();
		}
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}

void app_main(void)
{
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);
	ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_PM_ENABLE
	/* full speed while there is work, 80 MHz when idle; no light sleep, so
	 * Wi-Fi and Ethernet stay fully awake and no frames are lost */
	esp_pm_config_t pm = {
		.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
		.min_freq_mhz = 80,
		.light_sleep_enable = false,
	};
	ESP_ERROR_CHECK(esp_pm_configure(&pm));
#endif

	settings_init();
	/* optional network built into the firmware (menuconfig), saved on first boot */
	if (settings_network_count() == 0 && CONFIG_WRG_WIFI_SSID[0] != '\0') {
		settings_add_network(CONFIG_WRG_WIFI_SSID, CONFIG_WRG_WIFI_PASSWORD);
	}

	bool requested = esp_reset_reason() == ESP_RST_SW && s_setup_request == SETUP_REQUEST;
	s_setup_request = 0;
	s_setup_mode = requested || settings_network_count() == 0;

	display_init();
	button_start(CONFIG_WRG_BUTTON_GPIO, long_press);
	serial_console_start();
	ESP_ERROR_CHECK(esp_netif_init());	/* lwIP: Telnet, setup web page */
	telnet_start();
	eventlog_add("started in %s mode", s_setup_mode ? "setup" : "bridge");

	if (s_setup_mode) {
		ESP_LOGI(TAG, "setup mode (%s)", requested ? "button" : "no saved networks");
		setup_run();	/* never returns */
	}

	ESP_LOGI(TAG, "bridge mode");
	xTaskCreate(status_task, "status", 4096, NULL, 2, NULL);
	bridge_start();
}
