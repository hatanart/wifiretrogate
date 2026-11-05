/*
 * Persistent settings in NVS. See settings.h.
 */
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "settings.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE	"wrg"

static wrg_network_t s_networks[WRG_MAX_NETWORKS];
static int s_network_count;
static char s_ap_password[9];

static esp_err_t save_networks(void)
{
	nvs_handle_t nvs;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
	if (err != ESP_OK) {
		return err;
	}
	err = nvs_set_blob(nvs, "networks", s_networks, s_network_count * sizeof(wrg_network_t));
	if (err == ESP_OK) {
		err = nvs_commit(nvs);
	}
	nvs_close(nvs);
	return err;
}

static void load_ap_password(nvs_handle_t nvs)
{
	/* no 0/o, 1/l/i: easy to read off a small display */
	static const char charset[] = "abcdefghjkmnpqrstuvwxyz23456789";
	size_t len = sizeof s_ap_password;

	if (nvs_get_str(nvs, "ap_password", s_ap_password, &len) == ESP_OK && strlen(s_ap_password) == 8) {
		return;
	}
	for (int i = 0; i < 8; i++) {
		s_ap_password[i] = charset[esp_random() % (sizeof charset - 1)];
	}
	s_ap_password[8] = '\0';
	nvs_set_str(nvs, "ap_password", s_ap_password);
	nvs_commit(nvs);
}

void settings_init(void)
{
	nvs_handle_t nvs;
	if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
		ESP_LOGE(TAG, "cannot open NVS");
		return;
	}

	size_t len = sizeof s_networks;
	if (nvs_get_blob(nvs, "networks", s_networks, &len) == ESP_OK) {
		s_network_count = len / sizeof(wrg_network_t);
	}
	load_ap_password(nvs);
	nvs_close(nvs);

	ESP_LOGI(TAG, "%d saved network(s)", s_network_count);
}

int settings_network_count(void)
{
	return s_network_count;
}

const wrg_network_t *settings_network(int index)
{
	return index >= 0 && index < s_network_count ? &s_networks[index] : NULL;
}

const wrg_network_t *settings_find_network(const char *ssid)
{
	for (int i = 0; i < s_network_count; i++) {
		if (strcmp(s_networks[i].ssid, ssid) == 0) {
			return &s_networks[i];
		}
	}
	return NULL;
}

esp_err_t settings_add_network(const char *ssid, const char *password)
{
	if (ssid[0] == '\0' || strlen(ssid) > WRG_SSID_MAX || strlen(password) > WRG_PASSWORD_MAX
	    || (password[0] != '\0' && strlen(password) < 8)) {
		return ESP_ERR_INVALID_ARG;	/* WPA passwords are 8..63 characters */
	}

	wrg_network_t *network = (wrg_network_t *) settings_find_network(ssid);
	if (network == NULL) {
		if (s_network_count == WRG_MAX_NETWORKS) {
			memmove(&s_networks[0], &s_networks[1], (WRG_MAX_NETWORKS - 1) * sizeof(wrg_network_t));
			s_network_count--;
		}
		network = &s_networks[s_network_count++];
		memset(network, 0, sizeof *network);
		strcpy(network->ssid, ssid);
	}
	strcpy(network->password, password);

	ESP_LOGI(TAG, "saved network \"%s\"", ssid);
	return save_networks();
}

esp_err_t settings_forget_network(const char *ssid)
{
	for (int i = 0; i < s_network_count; i++) {
		if (strcmp(s_networks[i].ssid, ssid) == 0) {
			memmove(&s_networks[i], &s_networks[i + 1], (s_network_count - i - 1) * sizeof(wrg_network_t));
			s_network_count--;
			ESP_LOGI(TAG, "forgot network \"%s\"", ssid);
			return save_networks();
		}
	}
	return ESP_ERR_NOT_FOUND;
}

bool settings_get_client_mac(uint8_t mac[6])
{
	nvs_handle_t nvs;
	size_t len = 6;
	if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
		return false;
	}
	esp_err_t err = nvs_get_blob(nvs, "client_mac", mac, &len);
	nvs_close(nvs);
	return err == ESP_OK && len == 6;
}

esp_err_t settings_set_client_mac(const uint8_t mac[6])
{
	nvs_handle_t nvs;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
	if (err != ESP_OK) {
		return err;
	}
	err = nvs_set_blob(nvs, "client_mac", mac, 6);
	if (err == ESP_OK) {
		err = nvs_commit(nvs);
	}
	nvs_close(nvs);
	return err;
}

esp_err_t settings_forget_client_mac(void)
{
	nvs_handle_t nvs;
	esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
	if (err != ESP_OK) {
		return err;
	}
	nvs_erase_key(nvs, "client_mac");
	err = nvs_commit(nvs);
	nvs_close(nvs);
	return err;
}

const char *settings_ap_password(void)
{
	return s_ap_password;
}
