/*
 * Wi-Fi station manager for bridge mode. See wifi_sta.h.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_private/wifi.h"
#include "eventlog.h"
#include "settings.h"
#include "wifi_sta.h"

static const char *TAG = "wifi";

#define CONNECTED_BIT		BIT0
#define DISCONNECTED_BIT	BIT1

#define MAX_SCAN_RESULTS	20
#define CONNECT_WAIT_MS		30000	/* safety net; the driver normally reports failure itself */
#define RESCAN_DELAY_MS		5000

volatile bool wifi_sta_up;

static EventGroupHandle_t s_events;
static wifi_rxcb_t s_rx;
static wifi_sta_status_t s_status;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_scan_lock;

esp_err_t wifi_scan(wifi_ap_record_t *records, uint16_t *count)
{
	taskENTER_CRITICAL(&s_lock);
	if (s_scan_lock == NULL) {
		s_scan_lock = xSemaphoreCreateMutex();
	}
	taskEXIT_CRITICAL(&s_lock);

	xSemaphoreTake(s_scan_lock, portMAX_DELAY);
	wifi_scan_config_t scan = { .show_hidden = true };
	esp_err_t err = esp_wifi_scan_start(&scan, true);
	if (err == ESP_OK) {
		err = esp_wifi_scan_get_ap_records(count, records);
	}
	xSemaphoreGive(s_scan_lock);
	if (err != ESP_OK) {
		*count = 0;
	}
	return err;
}

void wifi_sta_reconnect(void)
{
	esp_wifi_disconnect();
}

static void set_state(wifi_sta_state_t state, const char *ssid)
{
	taskENTER_CRITICAL(&s_lock);
	s_status.state = state;
	if (ssid != NULL) {
		strlcpy(s_status.ssid, ssid, sizeof s_status.ssid);
	}
	taskEXIT_CRITICAL(&s_lock);
}

const char *wifi_sta_reason_text(uint8_t reason)
{
	switch (reason) {
	case WIFI_REASON_NO_AP_FOUND:			return "not found";
	case WIFI_REASON_AUTH_FAIL:			return "auth failed";
	case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
	case WIFI_REASON_HANDSHAKE_TIMEOUT:		return "wrong password";
	case WIFI_REASON_ASSOC_FAIL:
	case WIFI_REASON_CONNECTION_FAIL:		return "connect failed";
	case WIFI_REASON_BEACON_TIMEOUT:		return "signal lost";
	default:					return "disconnected";
	}
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	if (id == WIFI_EVENT_STA_CONNECTED) {
		esp_wifi_internal_reg_rxcb(WIFI_IF_STA, s_rx);
		wifi_sta_up = true;
		xEventGroupSetBits(s_events, CONNECTED_BIT);
	} else if (id == WIFI_EVENT_STA_DISCONNECTED) {
		wifi_event_sta_disconnected_t *d = data;
		wifi_sta_up = false;
		esp_wifi_internal_reg_rxcb(WIFI_IF_STA, NULL);
		s_status.reason = d->reason;
		xEventGroupSetBits(s_events, DISCONNECTED_BIT);
	}
}

/* Scan and return saved networks in range, strongest first. Hidden networks
 * do not show up in a scan; saved networks that were not seen are appended
 * at the end so they still get tried. */
static int find_candidates(const wrg_network_t *candidates[WRG_MAX_NETWORKS])
{
	static wifi_ap_record_t records[MAX_SCAN_RESULTS];
	uint16_t count = MAX_SCAN_RESULTS;
	int found = 0;

	wifi_scan(records, &count);

	/* records are sorted by signal strength, strongest first */
	for (int r = 0; r < count; r++) {
		const wrg_network_t *network = settings_find_network((const char *) records[r].ssid);
		bool seen = false;
		for (int c = 0; c < found; c++) {
			seen |= candidates[c] == network;
		}
		if (network != NULL && !seen) {
			candidates[found++] = network;
		}
	}
	int in_range = found;

	for (int i = 0; i < settings_network_count(); i++) {
		const wrg_network_t *network = settings_network(i);
		bool seen = false;
		for (int c = 0; c < found; c++) {
			seen |= candidates[c] == network;
		}
		if (!seen) {
			candidates[found++] = network;
		}
	}

	ESP_LOGI(TAG, "scan: %u networks, %d saved in range", count, in_range);
	return in_range > 0 ? found : 0;
}

static bool try_network(const wrg_network_t *network)
{
	wifi_config_t config = { 0 };
	strlcpy((char *) config.sta.ssid, network->ssid, sizeof config.sta.ssid);
	strlcpy((char *) config.sta.password, network->password, sizeof config.sta.password);
	/* WPA2 threshold also accepts WPA3 and WPA2/WPA3 mixed networks */
	config.sta.threshold.authmode = network->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
	config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
	config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
	config.sta.pmf_cfg.capable = true;

	ESP_LOGI(TAG, "connecting to \"%s\"", network->ssid);
	set_state(WIFI_STA_CONNECTING, network->ssid);
	xEventGroupClearBits(s_events, CONNECTED_BIT | DISCONNECTED_BIT);
	esp_wifi_set_config(WIFI_IF_STA, &config);
	esp_wifi_connect();

	EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT | DISCONNECTED_BIT,
					       pdTRUE, pdFALSE, pdMS_TO_TICKS(CONNECT_WAIT_MS));
	if (bits & CONNECTED_BIT) {
		return true;
	}
	if (!(bits & DISCONNECTED_BIT)) {
		esp_wifi_disconnect();
	}
	ESP_LOGW(TAG, "\"%s\": %s (reason %u)", network->ssid,
		 wifi_sta_reason_text(s_status.reason), s_status.reason);
	eventlog_add("%.20s: %s (%u)", network->ssid, wifi_sta_reason_text(s_status.reason), s_status.reason);
	set_state(WIFI_STA_FAILED, NULL);
	return false;
}

static void wifi_task(void *arg)
{
	const wrg_network_t *candidates[WRG_MAX_NETWORKS];

	for (;;) {
		set_state(WIFI_STA_SEARCHING, NULL);
		int count = find_candidates(candidates);
		if (count == 0) {
			set_state(WIFI_STA_NOT_FOUND, "");
			vTaskDelay(pdMS_TO_TICKS(RESCAN_DELAY_MS));
			continue;
		}

		for (int i = 0; i < count; i++) {
			if (!try_network(candidates[i])) {
				continue;
			}

			ESP_LOGI(TAG, "connected to \"%s\"", candidates[i]->ssid);
			eventlog_add("Wi-Fi connected: %.24s", candidates[i]->ssid);
			set_state(WIFI_STA_CONNECTED, candidates[i]->ssid);

			/* stay here until the connection drops, then search again */
			xEventGroupWaitBits(s_events, DISCONNECTED_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
			ESP_LOGW(TAG, "connection to \"%s\" lost: %s", candidates[i]->ssid,
				 wifi_sta_reason_text(s_status.reason));
			eventlog_add("Wi-Fi lost: %s (%u)", wifi_sta_reason_text(s_status.reason), s_status.reason);
			break;
		}

		/* failed attempts already got a "FAILED" state; give the display a moment */
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}

void wifi_sta_start(const uint8_t mac[6], wifi_rxcb_t rx)
{
	s_events = xEventGroupCreate();
	s_rx = rx;

	wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&init));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	/* the station takes over the client's identity; must be set before start */
	ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, mac));
	ESP_ERROR_CHECK(esp_wifi_start());
	/* power save makes the station miss broadcasts (IPX discovery breaks) */
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

	xTaskCreate(wifi_task, "wifi_sta", 4096, NULL, 5, NULL);
}

void wifi_sta_get_status(wifi_sta_status_t *status)
{
	taskENTER_CRITICAL(&s_lock);
	*status = s_status;
	taskEXIT_CRITICAL(&s_lock);

	status->rssi = 0;
	status->channel = 0;
	wifi_ap_record_t ap;
	if (status->state == WIFI_STA_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
		status->rssi = ap.rssi;
		status->channel = ap.primary;
	}
}
