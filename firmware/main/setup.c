/*
 * Setup mode. See setup.h.
 *
 * The device opens a WPA2 access point "WifiRetroGate-XXXX" whose password is
 * shown on the display. Without a display nobody could read the password, so
 * the access point is then open (only while setup mode is on). Phones that join get the web page at
 * http://192.168.4.1/ automatically: every DNS name resolves to us and every
 * unknown URL redirects to the page (captive portal).
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "display.h"
#include "eventlog.h"
#include "settings.h"
#include "wifi_sta.h"
#include "wrg.h"
#include "setup.h"

static const char *TAG = "setup";

#define MAX_SCAN_RESULTS	20

static char s_ap_ssid[33];
static char s_message[64] = "Waiting for phone";
static volatile bool s_finish;

static wifi_ap_record_t s_scan[MAX_SCAN_RESULTS];
static uint16_t s_scan_count;

/* ---------- helpers ---------- */

static void scan_networks(void)
{
	uint16_t count = MAX_SCAN_RESULTS;

	wifi_scan(s_scan, &count);
	s_scan_count = count;
	ESP_LOGI(TAG, "scan: %u networks", s_scan_count);
}

/* In-place decoding of application/x-www-form-urlencoded values */
static void url_decode(char *s)
{
	char *out = s;

	for (; *s; s++) {
		if (*s == '+') {
			*out++ = ' ';
		} else if (*s == '%' && isxdigit((unsigned char) s[1]) && isxdigit((unsigned char) s[2])) {
			char hex[3] = { s[1], s[2], '\0' };
			*out++ = (char) strtol(hex, NULL, 16);
			s += 2;
		} else {
			*out++ = *s;
		}
	}
	*out = '\0';
}

static bool form_value(const char *body, const char *key, char *value, size_t size)
{
	if (httpd_query_key_value(body, key, value, size) != ESP_OK) {
		value[0] = '\0';
		return false;
	}
	url_decode(value);
	return true;
}

static esp_err_t read_body(httpd_req_t *req, char *body, size_t size)
{
	size_t total = 0;

	if (req->content_len >= size) {
		return ESP_FAIL;
	}
	while (total < req->content_len) {
		int n = httpd_req_recv(req, body + total, req->content_len - total);
		if (n <= 0) {
			return ESP_FAIL;
		}
		total += n;
	}
	body[total] = '\0';
	return ESP_OK;
}

static void send_escaped(httpd_req_t *req, const char *text)
{
	char buffer[160];
	size_t n = 0;

	for (; *text && n < sizeof buffer - 7; text++) {
		const char *entity = NULL;
		switch (*text) {
		case '&': entity = "&amp;"; break;
		case '<': entity = "&lt;"; break;
		case '>': entity = "&gt;"; break;
		case '"': entity = "&quot;"; break;
		case '\'': entity = "&#39;"; break;
		}
		if (entity) {
			n += strlcpy(buffer + n, entity, sizeof buffer - n);
		} else {
			buffer[n++] = *text;
		}
	}
	buffer[n] = '\0';
	httpd_resp_sendstr_chunk(req, buffer);
}

static esp_err_t redirect_home(httpd_req_t *req)
{
	httpd_resp_set_status(req, "303 See Other");
	httpd_resp_set_hdr(req, "Location", "/");
	return httpd_resp_send(req, NULL, 0);
}

/* ---------- web page ---------- */

static const char PAGE_HEAD[] =
	"<!DOCTYPE html><html><head><meta charset=utf-8>"
	"<meta name=viewport content='width=device-width,initial-scale=1'>"
	"<title>WifiRetroGate setup</title><style>"
	"body{font-family:sans-serif;max-width:30em;margin:1em auto;padding:0 1em}"
	"input,button{font-size:1em;width:100%;box-sizing:border-box;padding:.5em;margin:.2em 0 .6em}"
	"table{width:100%;border-collapse:collapse}td{padding:.3em 0;border-bottom:1px solid #ddd}"
	"td.r{text-align:right}.msg{background:#eef;padding:.5em}"
	"form.inline{margin:0}form.inline button{width:auto;margin:0;padding:.2em .6em}"
	"</style></head><body><h1>WifiRetroGate</h1>";

static esp_err_t page_get(httpd_req_t *req)
{
	char query[32];
	char line[96];

	if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK && strstr(query, "scan")) {
		scan_networks();
	}

	httpd_resp_set_type(req, "text/html");
	httpd_resp_sendstr_chunk(req, PAGE_HEAD);

	httpd_resp_sendstr_chunk(req, "<p class=msg>");
	send_escaped(req, s_message);
	httpd_resp_sendstr_chunk(req, "</p>");

	/* add a network: pick from the scan or type a name (hidden networks) */
	httpd_resp_sendstr_chunk(req,
		"<h2>Add a network</h2><form method=post action=/add>"
		"<label>Network name<input name=ssid list=nets required maxlength=32 autocapitalize=none></label>"
		"<datalist id=nets>");
	for (int i = 0; i < s_scan_count; i++) {
		if (s_scan[i].ssid[0] == '\0') {
			continue;	/* hidden */
		}
		httpd_resp_sendstr_chunk(req, "<option value=\"");
		send_escaped(req, (const char *) s_scan[i].ssid);
		httpd_resp_sendstr_chunk(req, "\">");
	}
	httpd_resp_sendstr_chunk(req,
		"</datalist><label>Password (empty for an open network)"
		"<input name=password type=password maxlength=63></label>"
		"<button>Save</button></form>");

	/* networks in range */
	httpd_resp_sendstr_chunk(req, "<h2>Networks in range</h2><table>");
	for (int i = 0; i < s_scan_count; i++) {
		if (s_scan[i].ssid[0] == '\0') {
			continue;	/* hidden */
		}
		httpd_resp_sendstr_chunk(req, "<tr><td>");
		send_escaped(req, (const char *) s_scan[i].ssid);
		snprintf(line, sizeof line, "</td><td class=r>%s ch %u, %d dBm</td></tr>",
			 s_scan[i].authmode == WIFI_AUTH_OPEN ? "open," : "",
			 s_scan[i].primary, s_scan[i].rssi);
		httpd_resp_sendstr_chunk(req, line);
	}
	httpd_resp_sendstr_chunk(req, "</table><p><a href='/?scan=1'>Scan again</a></p>");

	/* saved networks */
	httpd_resp_sendstr_chunk(req, "<h2>Saved networks</h2><table>");
	if (settings_network_count() == 0) {
		httpd_resp_sendstr_chunk(req, "<tr><td>none yet</td></tr>");
	}
	for (int i = 0; i < settings_network_count(); i++) {
		const wrg_network_t *network = settings_network(i);
		httpd_resp_sendstr_chunk(req, "<tr><td>");
		send_escaped(req, network->ssid);
		httpd_resp_sendstr_chunk(req, network->password[0] ? "" : " (open)");
		httpd_resp_sendstr_chunk(req,
			"</td><td class=r><form class=inline method=post action=/forget>"
			"<input type=hidden name=ssid value=\"");
		send_escaped(req, network->ssid);
		httpd_resp_sendstr_chunk(req, "\"><button>Forget</button></form></td></tr>");
	}
	httpd_resp_sendstr_chunk(req, "</table>");

	httpd_resp_sendstr_chunk(req,
		"<h2>Done</h2><form method=post action=/finish>"
		"<button>Finish and connect</button></form></body></html>");
	return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t add_post(httpd_req_t *req)
{
	char body[512], ssid[WRG_SSID_MAX * 3 + 1], password[WRG_PASSWORD_MAX * 3 + 1];

	if (read_body(req, body, sizeof body) != ESP_OK) {
		return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
	}
	form_value(body, "ssid", ssid, sizeof ssid);
	form_value(body, "password", password, sizeof password);

	esp_err_t err = settings_add_network(ssid, password);
	if (err == ESP_OK) {
		snprintf(s_message, sizeof s_message, "Saved %.40s", ssid);
	} else if (password[0] != '\0' && strlen(password) < 8) {
		snprintf(s_message, sizeof s_message, "Password too short (min 8)");
	} else {
		snprintf(s_message, sizeof s_message, "Could not save network");
	}
	return redirect_home(req);
}

static esp_err_t forget_post(httpd_req_t *req)
{
	char body[128], ssid[WRG_SSID_MAX * 3 + 1];

	if (read_body(req, body, sizeof body) != ESP_OK) {
		return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
	}
	form_value(body, "ssid", ssid, sizeof ssid);
	if (settings_forget_network(ssid) == ESP_OK) {
		snprintf(s_message, sizeof s_message, "Forgot %.40s", ssid);
	}
	return redirect_home(req);
}

static esp_err_t finish_post(httpd_req_t *req)
{
	httpd_resp_set_type(req, "text/html");
	httpd_resp_sendstr_chunk(req, PAGE_HEAD);
	httpd_resp_sendstr_chunk(req, settings_network_count() > 0
		? "<p>Connecting. You can close this page; the display shows the status.</p>"
		: "<p>No networks saved. Restarting setup.</p>");
	httpd_resp_sendstr_chunk(req, "</body></html>");
	httpd_resp_sendstr_chunk(req, NULL);
	s_finish = true;
	return ESP_OK;
}

/* Captive portal: everything unknown goes to the setup page */
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t error)
{
	httpd_resp_set_status(req, "302 Found");
	httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
	return httpd_resp_send(req, NULL, 0);
}

static void start_web_server(void)
{
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.lru_purge_enable = true;		/* phones open many connections */
	config.stack_size = 6144;

	httpd_handle_t server;
	ESP_ERROR_CHECK(httpd_start(&server, &config));

	const httpd_uri_t uris[] = {
		{ .uri = "/", .method = HTTP_GET, .handler = page_get },
		{ .uri = "/add", .method = HTTP_POST, .handler = add_post },
		{ .uri = "/forget", .method = HTTP_POST, .handler = forget_post },
		{ .uri = "/finish", .method = HTTP_POST, .handler = finish_post },
	};
	for (int i = 0; i < sizeof uris / sizeof uris[0]; i++) {
		httpd_register_uri_handler(server, &uris[i]);
	}
	httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
}

/* ---------- DNS: every name resolves to 192.168.4.1 ---------- */

static void dns_task(void *arg)
{
	uint8_t packet[512];
	int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(53),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	if (sock < 0 || bind(sock, (struct sockaddr *) &addr, sizeof addr) < 0) {
		ESP_LOGE(TAG, "DNS server: cannot bind port 53");
		vTaskDelete(NULL);
	}

	for (;;) {
		struct sockaddr_in from;
		socklen_t from_len = sizeof from;
		int len = recvfrom(sock, packet, sizeof packet - 16, 0, (struct sockaddr *) &from, &from_len);

		/* a standard query with exactly one question */
		if (len < 12 || (packet[2] & 0x80) || packet[4] != 0 || packet[5] != 1) {
			continue;
		}
		int end = 12;
		while (end < len && packet[end] != 0) {
			end += packet[end] + 1;
		}
		end += 1 + 4;		/* terminating zero, type, class */
		if (end > len) {
			continue;
		}
		bool type_a = packet[end - 4] == 0 && packet[end - 3] == 1;

		packet[2] = 0x81;	/* response, recursion desired */
		packet[3] = 0x80;	/* recursion available, no error */
		packet[6] = 0;		/* answer count */
		packet[7] = type_a ? 1 : 0;
		memset(packet + 8, 0, 4);	/* no authority or additional records */

		if (type_a) {
			static const uint8_t answer[] = {
				0xc0, 0x0c,		/* name: pointer to the question */
				0x00, 0x01, 0x00, 0x01,	/* type A, class IN */
				0x00, 0x00, 0x00, 0x3c,	/* TTL 60 s */
				0x00, 0x04, 192, 168, 4, 1,
			};
			memcpy(packet + end, answer, sizeof answer);
			end += sizeof answer;
		}
		sendto(sock, packet, end, 0, (struct sockaddr *) &from, from_len);
	}
}

/* ---------- setup mode ---------- */

void setup_run(void)
{
	esp_netif_create_default_wifi_ap();

	wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&init));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));	/* STA only for scanning */

	uint8_t mac[6];
	esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
	snprintf(s_ap_ssid, sizeof s_ap_ssid, "WifiRetroGate-%02X%02X", mac[4], mac[5]);

	wifi_config_t config = {
		.ap = {
			.channel = 1,
			.max_connection = 2,
			.authmode = WIFI_AUTH_WPA2_PSK,
		},
	};
	const char *ap_password = display_present() ? settings_ap_password() : "";
	strlcpy((char *) config.ap.ssid, s_ap_ssid, sizeof config.ap.ssid);
	config.ap.ssid_len = strlen(s_ap_ssid);
	if (ap_password[0]) {
		strlcpy((char *) config.ap.password, ap_password, sizeof config.ap.password);
	} else {
		config.ap.authmode = WIFI_AUTH_OPEN;
	}
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
	ESP_ERROR_CHECK(esp_wifi_start());

	if (ap_password[0]) {
		ESP_LOGI(TAG, "access point \"%s\", password \"%s\"", s_ap_ssid, ap_password);
	} else {
		ESP_LOGI(TAG, "access point \"%s\", open (no display found)", s_ap_ssid);
	}
	eventlog_add("setup access point %s, %s", s_ap_ssid, ap_password[0] ? "WPA2" : "open (no display)");
	display_setup(s_ap_ssid, ap_password, "Scanning...");
	scan_networks();

	start_web_server();
	xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);

	strlcpy(s_message, settings_network_count() ? "Add or finish" : "Add a network", sizeof s_message);
	while (!s_finish) {
		display_setup(s_ap_ssid, ap_password, s_message);
		vTaskDelay(pdMS_TO_TICKS(1000));
	}

	vTaskDelay(pdMS_TO_TICKS(1000));	/* let the last page reach the phone */
	wrg_restart(false);
}
