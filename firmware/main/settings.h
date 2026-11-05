/*
 * Persistent settings in NVS: saved Wi-Fi networks, client MAC,
 * setup AP password.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define WRG_MAX_NETWORKS	8
#define WRG_SSID_MAX		32
#define WRG_PASSWORD_MAX	64

typedef struct {
	char ssid[WRG_SSID_MAX + 1];
	char password[WRG_PASSWORD_MAX + 1];	/* empty = open network */
} wrg_network_t;

/* Load settings from NVS (call after nvs_flash_init) */
void settings_init(void);

int settings_network_count(void);
const wrg_network_t *settings_network(int index);
const wrg_network_t *settings_find_network(const char *ssid);

/* Add a network, or update the password of a saved one. When the list is
 * full, the oldest network is dropped. */
esp_err_t settings_add_network(const char *ssid, const char *password);
esp_err_t settings_forget_network(const char *ssid);

bool settings_get_client_mac(uint8_t mac[6]);
esp_err_t settings_set_client_mac(const uint8_t mac[6]);
esp_err_t settings_forget_client_mac(void);

/* WPA2 password of the setup access point; generated once per device */
const char *settings_ap_password(void);
