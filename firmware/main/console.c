/*
 * Command interpreter. See console.h.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "bridge.h"
#include "console.h"
#include "eventlog.h"
#include "settings.h"
#include "setup.h"
#include "status.h"
#include "wifi_sta.h"
#include "wrg.h"

#define MAX_ARGS	8

static SemaphoreHandle_t s_lock;
static console_write_t *s_write;
static void *s_ctx;

static void out(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void out(const char *format, ...)
{
	char text[160];
	va_list args;

	va_start(args, format);
	vsnprintf(text, sizeof text, format, args);
	va_end(args);
	s_write(s_ctx, text);
}

static void out_line(void *ctx, const char *line)
{
	out("%s\n", line);
}

/* Split a line into words; "double quotes" keep spaces (network names). */
static int split(char *line, char *argv[MAX_ARGS])
{
	int argc = 0;
	char *p = line;

	while (*p && argc < MAX_ARGS) {
		while (*p == ' ' || *p == '\t') {
			p++;
		}
		if (*p == '\0') {
			break;
		}
		if (*p == '"') {
			argv[argc++] = ++p;
			while (*p && *p != '"') {
				p++;
			}
		} else {
			argv[argc++] = p;
			while (*p && *p != ' ' && *p != '\t') {
				p++;
			}
		}
		if (*p) {
			*p++ = '\0';
		}
	}
	return argc;
}

static bool parse_mac(const char *text, uint8_t mac[6])
{
	unsigned v[6];
	if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6
	    && sscanf(text, "%2x%2x%2x%2x%2x%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
		return false;
	}
	for (int i = 0; i < 6; i++) {
		mac[i] = v[i];
	}
	return !(mac[0] & 0x01);
}

/* ---------- commands ---------- */

static void cmd_help(int argc, char **argv)
{
	/* longer than out()'s buffer: written as is */
	s_write(s_ctx, "status              current state\n"
	    "counters            frame counters\n"
	    "scan                Wi-Fi networks in range\n"
	    "log                 recent events\n"
	    "macs                machines seen on Ethernet\n"
	    "wifi list           saved networks\n"
	    "wifi add NAME [PW]  save a network (quote names with spaces)\n"
	    "wifi forget NAME    forget a network\n"
	    "reconnect           drop Wi-Fi and search again\n"
	    "client              client MAC; client forget | client set MAC\n"
	    "setup               restart into setup mode (access point)\n"
	    "version             firmware version\n"
	    "reboot              restart in bridge mode\n"
	    "factory-reset       erase all settings and restart\n");
}

static void cmd_status(int argc, char **argv)
{
	if (wrg_setup_mode()) {
		out("SETUP MODE (access point)\n"
		    "add a network with: wifi add NAME PASSWORD, then: reboot\n");
		return;
	}

	char line[32];
	bridge_status_t b;
	wifi_sta_status_t w;
	status_line(line, sizeof line);
	bridge_get_status(&b);
	wifi_sta_get_status(&w);

	out("%s\n", line);
	if (b.eth_up) {
		out("ETH     link %s %s duplex\n", b.eth_100m ? "100M" : "10M", b.eth_full_duplex ? "full" : "half");
	} else {
		out("ETH     no link\n");
	}
	if (b.client_known) {
		out("CLIENT  %02x:%02x:%02x:%02x:%02x:%02x\n", b.client_mac[0], b.client_mac[1],
		    b.client_mac[2], b.client_mac[3], b.client_mac[4], b.client_mac[5]);
	} else {
		out("CLIENT  not known yet\n");
	}
	uint32_t ips[BRIDGE_MAX_CLIENT_IPS];
	int ip_count = bridge_get_client_ips(ips, BRIDGE_MAX_CLIENT_IPS);
	if (ip_count == 0) {
		out("IP      not seen yet\n");
	}
	for (int i = 0; i < ip_count; i++) {
		const uint8_t *a = (const uint8_t *) &ips[i];
		out("%s%u.%u.%u.%u%s\n", i == 0 ? "IP      " : "        ", a[0], a[1], a[2], a[3],
		    i == 0 && ip_count > 1 ? "  (newest)" : "");
	}
	if (b.asks_known) {
		const uint8_t *a = (const uint8_t *) &b.asks_ip;
		out("ASKS    %u.%u.%u.%u (last ARP request, %lu s ago)\n", a[0], a[1], a[2], a[3],
		    (unsigned long) b.asks_age_s);
	}
	if (b.gateway_stand_in) {
		if (b.stand_in_count > 0) {
			const uint8_t *a = (const uint8_t *) &b.stand_in_ip;
			out("GATEWAY no Wi-Fi since start: answering the client's ARP (%u.%u.%u.%u%s),\n"
			    "        so telnet %s works; fixed up when Wi-Fi connects\n",
			    a[0], a[1], a[2], a[3], b.stand_in_count > 1 ? " and others" : "",
			    CONFIG_WRG_SERVICE_IP);
		} else {
			out("GATEWAY no Wi-Fi since start: will answer the client's ARP for its gateway\n");
		}
	}
	if (b.dhcp_no_answer) {
		out("DHCP    client asks, no answer for %lu s%s\n", (unsigned long) b.dhcp_waiting_s,
		    w.state == WIFI_STA_CONNECTED ? " (network without DHCP?)" : " (Wi-Fi not connected)");
	}
	if (w.state == WIFI_STA_CONNECTED) {
		out("WIFI    %s, channel %u, %d dBm\n", w.ssid, w.channel, w.rssi);
	} else {
		out("WIFI    not connected%s%s\n", w.ssid[0] ? ", last tried " : "", w.ssid);
		if (w.state == WIFI_STA_NOT_FOUND) {
			out("        no saved network in range (phone hotspot? set it to 2.4 GHz)\n");
		}
	}
	if (b.ipx_ethernet_ii) {
		out("IPX     client sends Ethernet II: other WifiRetroGates get it as is,\n"
		    "        everyone else as SNAP. Use FRAME Ethernet_802.2 (the ODI default).\n");
	}
	out("TX %lu  RX %lu  DROP %lu  ERR %lu\n", (unsigned long) b.eth_to_wifi,
	    (unsigned long) b.wifi_to_eth,
	    (unsigned long) (b.drop_other_mac + b.drop_no_wifi + b.drop_no_eth),
	    (unsigned long) (b.err_wifi_tx + b.err_eth_tx));
}

static void cmd_counters(int argc, char **argv)
{
	bridge_status_t b;
	bridge_get_status(&b);

	out("ETH -> WIFI      %lu\n", (unsigned long) b.eth_to_wifi);
	out("WIFI -> ETH      %lu\n", (unsigned long) b.wifi_to_eth);
	out("drop other MAC   %lu  (frames from machines other than the client)\n", (unsigned long) b.drop_other_mac);
	out("drop echo        %lu  (client's own broadcasts repeated by the AP)\n", (unsigned long) b.drop_echo);
	out("drop no Wi-Fi    %lu\n", (unsigned long) b.drop_no_wifi);
	out("drop no ETH      %lu\n", (unsigned long) b.drop_no_eth);
	out("Wi-Fi TX full    %lu  (Wi-Fi slower than Ethernet)\n", (unsigned long) b.err_wifi_tx);
	out("ETH TX full      %lu  (Ethernet slower than Wi-Fi)\n", (unsigned long) b.err_eth_tx);
	out("802.1H out/in    %lu / %lu  (IPX/AARP Ethernet II: extra bridge-tunnel copies sent / converted)\n",
	    (unsigned long) b.fix_tx_8021h, (unsigned long) b.fix_rx_8021h);
	out("length repaired  %lu  (802.3 frames from Wi-Fi with length 0)\n", (unsigned long) b.fix_rx_length);
}

static const char *auth_text(wifi_auth_mode_t mode)
{
	switch (mode) {
	case WIFI_AUTH_OPEN:		return "open";
	case WIFI_AUTH_WEP:		return "WEP";
	case WIFI_AUTH_WPA_PSK:		return "WPA";
	case WIFI_AUTH_WPA2_PSK:	return "WPA2";
	case WIFI_AUTH_WPA_WPA2_PSK:	return "WPA/WPA2";
	case WIFI_AUTH_WPA3_PSK:	return "WPA3";
	case WIFI_AUTH_WPA2_WPA3_PSK:	return "WPA2/WPA3";
	default:			return "other";
	}
}

static void cmd_scan(int argc, char **argv)
{
	static wifi_ap_record_t records[20];
	uint16_t count = sizeof records / sizeof records[0];

	if (wifi_scan(records, &count) != ESP_OK) {
		out("scan failed (Wi-Fi not started yet?)\n");
		return;
	}
	out("SIGNAL  CH  SECURITY   NAME\n");
	for (int i = 0; i < count; i++) {
		const char *name = (const char *) records[i].ssid;
		bool saved = name[0] && settings_find_network(name) != NULL;
		out("%4d    %2u  %-9s  %s%s\n", records[i].rssi, records[i].primary,
		    auth_text(records[i].authmode), name[0] ? name : "(hidden)", saved ? "  [saved]" : "");
	}
}

static void cmd_log(int argc, char **argv)
{
	eventlog_dump(out_line, NULL);
}

static void cmd_macs(int argc, char **argv)
{
	bridge_other_mac_t macs[BRIDGE_MAX_OTHER_MACS];
	int count = bridge_get_other_macs(macs, BRIDGE_MAX_OTHER_MACS);

	if (count == 0) {
		out("only the client has been seen on Ethernet\n");
		return;
	}
	out("other machines on Ethernet (switch? loop?):\n");
	for (int i = 0; i < count; i++) {
		const uint8_t *m = macs[i].mac;
		out("  %02x:%02x:%02x:%02x:%02x:%02x  %lu frames\n", m[0], m[1], m[2], m[3], m[4], m[5],
		    (unsigned long) macs[i].frames);
	}
}

static void cmd_wifi(int argc, char **argv)
{
	if (argc < 2 || strcmp(argv[1], "list") == 0) {
		if (settings_network_count() == 0) {
			out("no saved networks\n");
		}
		for (int i = 0; i < settings_network_count(); i++) {
			const wrg_network_t *n = settings_network(i);
			out("  %s%s\n", n->ssid, n->password[0] ? "" : "  (open)");
		}
	} else if (strcmp(argv[1], "add") == 0 && argc >= 3) {
		const char *password = argc >= 4 ? argv[3] : "";
		esp_err_t err = settings_add_network(argv[2], password);
		if (err == ESP_OK) {
			out("saved \"%s\"\n", argv[2]);
		} else if (password[0] && strlen(password) < 8) {
			out("password too short (WPA needs 8..63 characters)\n");
		} else {
			out("could not save\n");
		}
	} else if (strcmp(argv[1], "forget") == 0 && argc >= 3) {
		out(settings_forget_network(argv[2]) == ESP_OK ? "forgotten\n" : "not saved\n");
	} else {
		out("usage: wifi list | wifi add NAME [PASSWORD] | wifi forget NAME\n");
	}
}

static void cmd_reconnect(int argc, char **argv)
{
	wifi_sta_reconnect();
	out("reconnecting\n");
}

static void cmd_client(int argc, char **argv)
{
	uint8_t mac[6];

	if (argc >= 2 && strcmp(argv[1], "forget") == 0) {
		settings_forget_client_mac();
		out("forgotten; restarting to learn a new client\n");
		vTaskDelay(pdMS_TO_TICKS(500));
		wrg_restart(false);
	} else if (argc >= 3 && strcmp(argv[1], "set") == 0) {
		if (!parse_mac(argv[2], mac)) {
			out("invalid MAC (use 00:80:c8:12:34:56)\n");
			return;
		}
		settings_set_client_mac(mac);
		out("saved; restarting\n");
		vTaskDelay(pdMS_TO_TICKS(500));
		wrg_restart(false);
	} else if (settings_get_client_mac(mac)) {
		out("client %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	} else {
		out("no client learned yet\n");
	}
}

static void cmd_setup(int argc, char **argv)
{
	out("restarting into setup mode\n");
	vTaskDelay(pdMS_TO_TICKS(500));
	wrg_restart(true);
}

static void cmd_version(int argc, char **argv)
{
	const esp_app_desc_t *app = esp_app_get_description();
	unsigned seconds = esp_timer_get_time() / 1000000;

	out("WifiRetroGate %s, built %s %s, ESP-IDF %s\n", app->version, app->date, app->time, app->idf_ver);
	out("up %u:%02u:%02u\n", seconds / 3600, seconds / 60 % 60, seconds % 60);
	out("free RAM %u KB (lowest so far %u KB, largest block %u KB)\n",
	    (unsigned) (esp_get_free_heap_size() / 1024),
	    (unsigned) (esp_get_minimum_free_heap_size() / 1024),
	    (unsigned) (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024));
#if CONFIG_PM_ENABLE
	out("CPU %d MHz when busy, 80 MHz when idle\n", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
#else
	out("CPU %d MHz\n", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
#endif
}

static void cmd_reboot(int argc, char **argv)
{
	out("restarting\n");
	if (wrg_setup_mode()) {
		/* the setup DHCP lease is no good on the real network */
		out("then get a new IP address (run DHCP again)\n");
	}
	vTaskDelay(pdMS_TO_TICKS(500));
	wrg_restart(false);	/* bridge mode (back to setup if no networks) */
}

static void cmd_factory_reset(int argc, char **argv)
{
	out("erasing all settings\n");
	vTaskDelay(pdMS_TO_TICKS(500));
	nvs_flash_erase();
	wrg_restart(false);
}

static const struct {
	const char *name;
	void (*run)(int argc, char **argv);
} s_commands[] = {
	{ "help", cmd_help },
	{ "?", cmd_help },
	{ "status", cmd_status },
	{ "counters", cmd_counters },
	{ "scan", cmd_scan },
	{ "log", cmd_log },
	{ "macs", cmd_macs },
	{ "wifi", cmd_wifi },
	{ "reconnect", cmd_reconnect },
	{ "client", cmd_client },
	{ "setup", cmd_setup },
	{ "version", cmd_version },
	{ "reboot", cmd_reboot },
	{ "factory-reset", cmd_factory_reset },
};

void console_exec(const char *line, console_write_t *write, void *ctx)
{
	if (wrg_setup_mode()) {
		setup_activity();
	}
	char buffer[200];
	char *argv[MAX_ARGS];

	strlcpy(buffer, line, sizeof buffer);
	int argc = split(buffer, argv);
	if (argc == 0) {
		return;
	}

	static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;
	taskENTER_CRITICAL(&init_lock);
	if (s_lock == NULL) {
		s_lock = xSemaphoreCreateMutex();
	}
	taskEXIT_CRITICAL(&init_lock);

	xSemaphoreTake(s_lock, portMAX_DELAY);
	s_write = write;
	s_ctx = ctx;

	bool found = false;
	for (int i = 0; i < sizeof s_commands / sizeof s_commands[0]; i++) {
		if (strcasecmp(argv[0], s_commands[i].name) == 0) {
			s_commands[i].run(argc, argv);
			found = true;
			break;
		}
	}
	if (!found) {
		out("unknown command \"%s\"; try help\n", argv[0]);
	}

	xSemaphoreGive(s_lock);
}
