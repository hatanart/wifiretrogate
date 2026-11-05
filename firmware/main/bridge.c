/*
 * Ethernet side and frame forwarding. See bridge.h.
 *
 *   Ethernet (promiscuous) -- frames from client MAC --> Wi-Fi station
 *   Wi-Fi station          -- everything it receives --> Ethernet
 *
 * The client MAC is learned from the first valid frame on Ethernet and saved.
 * On later boots Wi-Fi starts immediately with the saved MAC. A new client is
 * accepted only after the Ethernet link went down and up (or after boot).
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_private/wifi.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "driver/gpio.h"
#include "bridge.h"
#include "eventlog.h"
#include "rawcfg.h"
#include "settings.h"
#include "svcnet.h"
#include "wifi_sta.h"
#include "wrg.h"

static const char *TAG = "bridge";

/* WT32-ETH01 board wiring (LAN8720 PHY) */
#define WT32_PHY_POWER_GPIO	16	/* enables the 50 MHz clock oscillator */
#define WT32_PHY_ADDR		1
#define WT32_MDC_GPIO		23
#define WT32_MDIO_GPIO		18
#define WT32_RMII_CLK_GPIO	0

#define CLIENT_LEARNED_BIT	BIT0

#define MACSTR_FMT "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC2ARGS(m) (m)[0], (m)[1], (m)[2], (m)[3], (m)[4], (m)[5]

static EventGroupHandle_t s_events;
static esp_eth_handle_t s_eth;

/* written by the driver tasks, read by the status display */
static volatile bridge_status_t s_status;
static volatile bool s_relearn = true;	/* new link (or boot): check for a new client */
static volatile TickType_t s_last_ipx_ethernet_ii;	/* 0 = never */
static bridge_other_mac_t s_other_macs[BRIDGE_MAX_OTHER_MACS];
static int s_other_mac_count;

static void note_other_mac(const uint8_t *mac)
{
	for (int i = 0; i < s_other_mac_count; i++) {
		if (memcmp(s_other_macs[i].mac, mac, 6) == 0) {
			s_other_macs[i].frames++;
			return;
		}
	}
	if (s_other_mac_count < BRIDGE_MAX_OTHER_MACS) {
		memcpy(s_other_macs[s_other_mac_count].mac, mac, 6);
		s_other_macs[s_other_mac_count].frames = 1;
		s_other_mac_count++;
		eventlog_add("other machine on ETH: %02x%02x%02x%02x%02x%02x",
			     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	}
}

static bool is_config_frame(const uint8_t *frame, uint32_t len)
{
	return len >= 14 && frame[12] == (WRG_ETHERTYPE >> 8) && frame[13] == (WRG_ETHERTYPE & 0xff);
}

/* A frame may teach us the client MAC only if its source is a unicast address
 * and it is not link-local switch traffic (STP, LLDP, LACP: 01:80:C2:00:00:0x). */
static bool learnable(const uint8_t *frame, uint32_t len)
{
	static const uint8_t link_local[5] = { 0x01, 0x80, 0xc2, 0x00, 0x00 };

	if (len < 14) {
		return false;
	}
	if (frame[6] & 0x01) {
		return false;
	}
	if (memcmp(frame, link_local, 5) == 0 && (frame[5] & 0xf0) == 0) {
		return false;
	}
	return true;
}

/*
 * 802.1H for IPX and AARP. These two EtherTypes should cross Wi-Fi with the
 * "bridge-tunnel" header (OUI 00-00-F8) instead of the RFC 1042 header, so
 * that the receiver can tell Ethernet II IPX from 802.2 SNAP IPX.
 *
 * Measured on the ESP32 (2026-10-15): the hardware wraps every frame in an
 * RFC 1042 header and treats bytes 12..13 as the EtherType even when they are
 * a length; the AP unwraps it again. So length frames (802.2, raw 802.3,
 * SNAP) pass unchanged, but a real bridge-tunnel header can never be sent.
 *  - sending Ethernet II IPX: the AP turns it into SNAP. We send it as is
 *    (wired machines and laptops get SNAP) and a second copy as a length
 *    frame with a bridge-tunnel header, which another WifiRetroGate turns
 *    back into Ethernet II. IPX stacks ignore the copy in the other format.
 *  - receiving: bridge-tunnel frames (from an AP that does 802.1H, or from
 *    another WifiRetroGate) arrive unconverted; we convert them here.
 */
static const uint8_t BRIDGE_TUNNEL[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0xf8 };

static bool needs_bridge_tunnel(const uint8_t *frame, uint32_t len)
{
	return len >= 14
	    && ((frame[12] == 0x81 && frame[13] == 0x37)	/* IPX */
		|| (frame[12] == 0x80 && frame[13] == 0xf3));	/* AppleTalk AARP */
}

/* Ethernet II -> 802.3 length frame with the bridge-tunnel header (+8 bytes) */
static uint32_t add_bridge_tunnel(const uint8_t *frame, uint32_t len, uint8_t *out)
{
	uint32_t payload = len - 14;

	memcpy(out, frame, 12);				/* destination, source */
	out[12] = (6 + 2 + payload) >> 8;		/* 802.3 length */
	out[13] = (6 + 2 + payload) & 0xff;
	memcpy(out + 14, BRIDGE_TUNNEL, 6);
	memcpy(out + 20, frame + 12, 2 + payload);	/* EtherType, payload */
	return len + 8;
}

/* 802.3 length frame with the bridge-tunnel header -> Ethernet II, in place.
 * Returns the offset of the converted frame, or 0 if it is not such a frame.
 * The 802.3 length field is not used: the ESP32 sets it to 0 (see
 * repair_length_field), and frames from Wi-Fi carry no padding. */
static uint32_t strip_bridge_tunnel(uint8_t *frame, uint32_t *len)
{
	static int logged;

	if (*len < 22 || frame[12] >= 0x06 || memcmp(frame + 14, BRIDGE_TUNNEL, 6) != 0) {
		return 0;
	}
	if (logged < 3) {
		logged++;
		ESP_LOGI(TAG, "bridge-tunnel frame from Wi-Fi: len %lu, length field %u, type %02x%02x",
			 (unsigned long) *len, frame[12] << 8 | frame[13], frame[20], frame[21]);
	}
	/* the EtherType already sits at bytes 20..21: move the addresses up */
	memmove(frame + 8, frame, 12);
	*len -= 8;
	return 8;
}

/* The ESP32 delivers 802.3 length frames from Wi-Fi with the length field set
 * to 0 (seen with 802.2 IPX from a wired machine). DOS IPX stacks rely on that
 * field and drop such frames. Frames from Wi-Fi carry no padding, so the
 * correct value is the frame length minus the header. */
static void repair_length_field(uint8_t *frame, uint32_t len)
{
	if (len < 14 || frame[12] >= 0x06) {
		return;		/* Ethernet II */
	}
	uint32_t field = frame[12] << 8 | frame[13];
	if (field == 0 || field > len - 14) {
		frame[12] = (len - 14) >> 8;
		frame[13] = (len - 14) & 0xff;
		s_status.fix_rx_length++;
	}
}

/* Ethernet driver RX task: called for every frame on the wire */
static esp_err_t eth_rx(esp_eth_handle_t eth, uint8_t *frame, uint32_t len, void *priv)
{
	const uint8_t *src = frame + 6;

	if (is_config_frame(frame, len)) {
		/* configuration tool: handled here, never forwarded to Wi-Fi */
		rawcfg_input(frame, len);
	} else if (!s_status.client_known) {
		if (learnable(frame, len)) {
			memcpy((uint8_t *) s_status.client_mac, src, 6);
			s_status.client_known = true;
			xEventGroupSetBits(s_events, CLIENT_LEARNED_BIT);
		}
	} else if (memcmp(src, (const uint8_t *) s_status.client_mac, 6) == 0) {
		s_relearn = false;
		if (svcnet_input(frame, len)) {
			/* for our own services (Telnet on the service address) */
		} else if (wifi_sta_up) {
			if (esp_wifi_internal_tx(WIFI_IF_STA, frame, len) == ESP_OK) {
				s_status.eth_to_wifi++;
			} else {
				s_status.err_wifi_tx++;
			}
			if (needs_bridge_tunnel(frame, len) && frame[12] == 0x81) {
				/* Ethernet II IPX reaches only other WifiRetroGates as
				 * Ethernet II; everyone else gets SNAP. Hint at 802.2. */
				if (s_last_ipx_ethernet_ii == 0) {
					eventlog_add("client sends Ethernet II IPX: use 802.2");
				}
				s_last_ipx_ethernet_ii = xTaskGetTickCount() | 1;
			}
			if (needs_bridge_tunnel(frame, len) && len <= 1514) {
				/* second copy for other WifiRetroGates (see 802.1H above);
				 * only this task sends here: a static buffer is fine */
				static uint8_t tunneled[1514 + 8];
				uint32_t tunneled_len = add_bridge_tunnel(frame, len, tunneled);
				if (esp_wifi_internal_tx(WIFI_IF_STA, tunneled, tunneled_len) == ESP_OK) {
					s_status.fix_tx_8021h++;
				}
			}
		} else {
			s_status.drop_no_wifi++;
		}
	} else if (s_relearn && learnable(frame, len)) {
		/* link went down and up and a different machine is talking: switch */
		ESP_LOGW(TAG, "new client " MACSTR_FMT ", restarting", MAC2ARGS(src));
		settings_set_client_mac(src);
		wrg_restart(false);	/* leaves Wi-Fi with the old MAC first */
	} else {
		s_status.drop_other_mac++;
		memcpy((uint8_t *) s_status.last_other_mac, src, 6);
		if (learnable(frame, len)) {
			note_other_mac(src);
		}
	}

	free(frame);
	return ESP_OK;
}

/* Wi-Fi driver task: called for every frame the station receives */
static esp_err_t wifi_rx(void *buffer, uint16_t buffer_len, void *eb)
{
	uint8_t *frame = buffer;
	uint32_t len = buffer_len;

	uint32_t offset = strip_bridge_tunnel(frame, &len);
	if (offset) {
		frame += offset;
		s_status.fix_rx_8021h++;
	}
	repair_length_field(frame, len);
	const uint8_t *src = frame + 6;

	if (memcmp(src, (const uint8_t *) s_status.client_mac, 6) == 0) {
		/* the AP echoing the client's own broadcast back to us */
		s_status.drop_echo++;
	} else if (is_config_frame(frame, len)) {
		/* configuration frames from the Wi-Fi side are never passed on */
	} else if (!s_status.eth_up) {
		s_status.drop_no_eth++;
	} else if (esp_eth_transmit(s_eth, frame, len) == ESP_OK) {
		s_status.wifi_to_eth++;
	} else {
		s_status.err_eth_tx++;
	}

	esp_wifi_internal_free_rx_buffer(eb);
	return ESP_OK;
}

static void eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	switch (id) {
	case ETHERNET_EVENT_CONNECTED: {
		eth_speed_t speed;
		eth_duplex_t duplex;
		esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &speed);
		esp_eth_ioctl(s_eth, ETH_CMD_G_DUPLEX_MODE, &duplex);
		s_status.eth_100m = speed == ETH_SPEED_100M;
		s_status.eth_full_duplex = duplex == ETH_DUPLEX_FULL;
		s_status.eth_up = true;
		ESP_LOGI(TAG, "ETH: link %s %s duplex", s_status.eth_100m ? "100M" : "10M",
			 s_status.eth_full_duplex ? "full" : "half");
		eventlog_add("ETH link %s %s", s_status.eth_100m ? "100M" : "10M",
			     s_status.eth_full_duplex ? "full" : "half");
		break;
	}

	case ETHERNET_EVENT_DISCONNECTED:
		ESP_LOGW(TAG, "ETH: no link");
		eventlog_add("ETH no link");
		s_status.eth_up = false;
		s_relearn = true;
		break;

	default:
		break;
	}
}

static void ethernet_init(void)
{
	/* the LAN8720's clock oscillator must run before the MAC is initialized */
	gpio_reset_pin(WT32_PHY_POWER_GPIO);
	gpio_set_direction(WT32_PHY_POWER_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_level(WT32_PHY_POWER_GPIO, 1);
	vTaskDelay(pdMS_TO_TICKS(10));

	eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
	eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
	emac_config.smi_gpio.mdc_num = WT32_MDC_GPIO;
	emac_config.smi_gpio.mdio_num = WT32_MDIO_GPIO;
	emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
	emac_config.clock_config.rmii.clock_gpio = WT32_RMII_CLK_GPIO;
	esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);

	eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
	phy_config.phy_addr = WT32_PHY_ADDR;
	phy_config.reset_gpio_num = -1;
	esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);

	esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
	ESP_ERROR_CHECK(esp_eth_driver_install(&config, &s_eth));
	ESP_ERROR_CHECK(esp_eth_update_input_path(s_eth, eth_rx, NULL));

	bool promiscuous = true;
	ESP_ERROR_CHECK(esp_eth_ioctl(s_eth, ETH_CMD_S_PROMISCUOUS, &promiscuous));

	ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event, NULL));
	ESP_ERROR_CHECK(esp_eth_start(s_eth));
}

void bridge_start(void)
{
	s_events = xEventGroupCreate();

	uint8_t mac[6];
	if (settings_get_client_mac(mac)) {
		memcpy((uint8_t *) s_status.client_mac, mac, 6);
		s_status.client_known = true;
		ESP_LOGI(TAG, "saved client " MACSTR_FMT, MAC2ARGS(mac));
		ethernet_init();
	} else {
		ESP_LOGI(TAG, "no saved client, waiting for the first frame on Ethernet");
		ethernet_init();
		xEventGroupWaitBits(s_events, CLIENT_LEARNED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
		memcpy(mac, (const uint8_t *) s_status.client_mac, 6);
		ESP_LOGI(TAG, "learned client " MACSTR_FMT, MAC2ARGS(mac));
		eventlog_add("learned client %02x%02x%02x%02x%02x%02x", MAC2ARGS(mac));
		settings_set_client_mac(mac);
	}

	svcnet_start(mac);
	wifi_sta_start(mac, wifi_rx);
}

void bridge_get_status(bridge_status_t *status)
{
	memcpy(status, (const void *) &s_status, sizeof *status);

	TickType_t last = s_last_ipx_ethernet_ii;
	status->ipx_ethernet_ii = last != 0 && xTaskGetTickCount() - last < pdMS_TO_TICKS(60000);
}

int bridge_get_other_macs(bridge_other_mac_t *macs, int max)
{
	int count = s_other_mac_count < max ? s_other_mac_count : max;
	memcpy(macs, s_other_macs, count * sizeof *macs);
	return count;
}

esp_err_t bridge_eth_send(const void *frame, size_t len)
{
	if (s_eth == NULL || !s_status.eth_up) {
		return ESP_ERR_INVALID_STATE;
	}
	return esp_eth_transmit(s_eth, (void *) frame, len);
}

void bridge_eth_mac(uint8_t mac[6])
{
	esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, mac);
}
