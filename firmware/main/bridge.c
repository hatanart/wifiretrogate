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
#include "esp_eth_netif_glue.h"
#include "esp_netif.h"
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

/* what the client looks for (its gateway, a server): tells which network it
 * expects, even when its own address is unclear */
static volatile uint32_t s_asks_ip;
static volatile TickType_t s_asks_seen;			/* 0 = never */

/*
 * Gateway stand-in: when no saved network could be reached since start-up,
 * answer the client's ARP requests with our own MAC, so a machine that has an
 * address (static, or an old lease) can still reach Telnet on the service
 * address. Never after the first Wi-Fi connection, so a router restart is not
 * covered (the real gateway comes back). When Wi-Fi does connect, we ask for
 * the same addresses on Wi-Fi as the client: the real replies go to the client
 * and replace our MAC in its ARP cache.
 */
#define STAND_IN_MAX	4
static struct {
	uint32_t target;	/* address the client asked for */
	uint32_t sender;	/* the client's own address in that request */
} s_stand_in[STAND_IN_MAX];
static volatile int s_stand_in_count;

/* DHCP from the client that nobody answers (Ethernet task writes the request
 * times, Wi-Fi task clears them on a reply) */
static volatile TickType_t s_dhcp_waiting_since;	/* 0 = not waiting */
static volatile TickType_t s_dhcp_last_request;
static bridge_other_mac_t s_other_macs[BRIDGE_MAX_OTHER_MACS];
static int s_other_mac_count;

/* client IPv4 addresses, see bridge_get_client_ips() */
#define CLIENT_IP_RECENT_MS	(10 * 60 * 1000)
static uint32_t s_client_ips[BRIDGE_MAX_CLIENT_IPS];
static TickType_t s_client_ip_seen[BRIDGE_MAX_CLIENT_IPS];
static int s_client_ip_count;
static bool s_client_ip_from_arp;

static void note_client_ip(uint32_t ip, bool from_arp)
{
	if (ip == 0 || ip == 0xffffffff) {
		return;		/* 0.0.0.0: ARP probe or DHCP before it has an address */
	}
	if (from_arp && !s_client_ip_from_arp) {
		/* ARP names only the machine's own addresses; IPv4 sources could
		 * be machines it routes for: forget those */
		s_client_ip_from_arp = true;
		s_client_ip_count = 0;
	} else if (!from_arp && s_client_ip_from_arp) {
		return;
	}

	TickType_t now = xTaskGetTickCount();
	int slot = 0;
	for (int i = 0; i < s_client_ip_count; i++) {
		if (s_client_ips[i] == ip) {
			s_client_ip_seen[i] = now;
			return;
		}
		if (now - s_client_ip_seen[i] > now - s_client_ip_seen[slot]) {
			slot = i;
		}
	}
	if (s_client_ip_count < BRIDGE_MAX_CLIENT_IPS) {
		slot = s_client_ip_count++;
	}
	s_client_ips[slot] = ip;
	s_client_ip_seen[slot] = now;
}

/* UDP ports of an IPv4 frame, false if it is not UDP; *payload: UDP data offset */
static bool udp_ports(const uint8_t *frame, uint32_t len, int *src_port, int *dst_port,
		      uint32_t *payload)
{
	if (len < 34 || frame[12] != 0x08 || frame[13] != 0x00 || (frame[14] >> 4) != 4
	    || frame[23] != 17) {
		return false;
	}
	uint32_t udp = 14 + (frame[14] & 0x0f) * 4;
	if (len < udp + 8) {
		return false;
	}
	*src_port = frame[udp] << 8 | frame[udp + 1];
	*dst_port = frame[udp + 2] << 8 | frame[udp + 3];
	*payload = udp + 8;
	return true;
}

/* frames from the client: DHCP requests */
static void watch_dhcp_request(const uint8_t *frame, uint32_t len)
{
	int src_port, dst_port;
	uint32_t payload;
	if (udp_ports(frame, len, &src_port, &dst_port, &payload) && src_port == 68 && dst_port == 67) {
		TickType_t now = xTaskGetTickCount() | 1;
		if (s_dhcp_waiting_since == 0) {
			s_dhcp_waiting_since = now;
		}
		s_dhcp_last_request = now;
	}
}

/* frames from Wi-Fi: a DHCP reply for the client ends the wait (other
 * machines' replies can be broadcast too: check the client hardware address) */
static void watch_dhcp_reply(const uint8_t *frame, uint32_t len)
{
	int src_port, dst_port;
	uint32_t payload;
	if (s_dhcp_waiting_since != 0 && udp_ports(frame, len, &src_port, &dst_port, &payload)
	    && src_port == 67 && dst_port == 68 && len >= payload + 28 + 6
	    && memcmp(frame + payload + 28, (const uint8_t *) s_status.client_mac, 6) == 0) {
		s_dhcp_waiting_since = 0;
	}
}

static void learn_client_ip(const uint8_t *frame, uint32_t len)
{
	uint32_t ip;

	if (frame[12] == 0x08 && frame[13] == 0x06 && len >= 42
	    && frame[14] == 0x00 && frame[15] == 0x01		/* Ethernet */
	    && frame[16] == 0x08 && frame[17] == 0x00		/* IPv4 */
	    && frame[18] == 6 && frame[19] == 4) {
		memcpy(&ip, frame + 28, 4);			/* sender IP */
		note_client_ip(ip, true);

		uint32_t target;
		memcpy(&target, frame + 38, 4);			/* target IP */
		if (frame[20] == 0x00 && frame[21] == 0x01	/* request */
		    && ip != 0					/* not a probe for its own address */
		    && target != ip && target != 0) {		/* not an announcement */
			s_asks_ip = target;
			s_asks_seen = xTaskGetTickCount() | 1;
		}
	} else if (frame[12] == 0x08 && frame[13] == 0x00 && len >= 34
		   && (frame[14] >> 4) == 4 && !s_client_ip_from_arp) {
		memcpy(&ip, frame + 26, 4);			/* source IP */
		note_client_ip(ip, false);
	}
}

/* Ethernet task: an ARP request from the client while Wi-Fi was never up.
 * True if answered (consumed). */
static bool stand_in_arp(const uint8_t *frame, uint32_t len)
{
	if (wifi_sta_up || !wifi_sta_unreachable_since_boot()
	    || len < 42 || frame[12] != 0x08 || frame[13] != 0x06
	    || frame[14] != 0x00 || frame[15] != 0x01 || frame[16] != 0x08 || frame[17] != 0x00
	    || frame[18] != 6 || frame[19] != 4 || frame[20] != 0x00 || frame[21] != 0x01) {
		return false;
	}
	uint32_t sender, target;
	memcpy(&sender, frame + 28, 4);
	memcpy(&target, frame + 38, 4);
	if (sender == 0 || target == 0 || target == sender) {
		return false;		/* probe or announcement: not a question for us */
	}

	int count = s_stand_in_count;
	bool known = false;
	for (int i = 0; i < count; i++) {
		known |= s_stand_in[i].target == target;
	}
	if (!known && count < STAND_IN_MAX) {
		s_stand_in[count].target = target;
		s_stand_in[count].sender = sender;
		s_stand_in_count = count + 1;
		const uint8_t *t = (const uint8_t *) &target;
		eventlog_add("no Wi-Fi: answering ARP for %u.%u.%u.%u", t[0], t[1], t[2], t[3]);
	}

	uint8_t own[6];
	bridge_eth_mac(own);
	uint8_t reply[60] = { 0 };
	memcpy(reply, frame + 6, 6);			/* to the client */
	memcpy(reply + 6, own, 6);
	reply[12] = 0x08;
	reply[13] = 0x06;
	memcpy(reply + 14, frame + 14, 6);		/* Ethernet, IPv4, 6, 4 */
	reply[20] = 0x00;
	reply[21] = 0x02;				/* reply */
	memcpy(reply + 22, own, 6);			/* sender: us... */
	memcpy(reply + 28, &target, 4);			/* ...as the address it asked for */
	memcpy(reply + 32, frame + 6, 6);		/* target: the client */
	memcpy(reply + 38, &sender, 4);
	bridge_eth_send(reply, sizeof reply);
	return true;
}

/* Wi-Fi task, after a connection: ask for the stood-in addresses as the
 * client, so the real replies correct its ARP cache */
static void wifi_connected(void)
{
	int count = s_stand_in_count;
	if (count == 0) {
		return;
	}
	for (int round = 0; round < 2; round++) {
		if (round > 0) {
			vTaskDelay(pdMS_TO_TICKS(1000));	/* in case the first went out too early */
		}
		for (int i = 0; i < count; i++) {
			uint8_t request[42] = { 0 };
			memset(request, 0xff, 6);
			memcpy(request + 6, (const uint8_t *) s_status.client_mac, 6);
			request[12] = 0x08;
			request[13] = 0x06;
			request[15] = 0x01;
			request[16] = 0x08;
			request[18] = 6;
			request[19] = 4;
			request[21] = 0x01;
			memcpy(request + 22, (const uint8_t *) s_status.client_mac, 6);
			memcpy(request + 28, &s_stand_in[i].sender, 4);
			memcpy(request + 38, &s_stand_in[i].target, 4);
			esp_wifi_internal_tx(WIFI_IF_STA, request, sizeof request);
		}
	}
	eventlog_add("Wi-Fi up: asked again for %d address(es) for the client", count);
}

int bridge_get_client_ips(uint32_t *ips, int max)
{
	/* a copy first: the Ethernet task may change the table meanwhile */
	uint32_t ip[BRIDGE_MAX_CLIENT_IPS];
	TickType_t seen[BRIDGE_MAX_CLIENT_IPS];
	int count = s_client_ip_count;
	memcpy(ip, s_client_ips, sizeof ip);
	memcpy(seen, s_client_ip_seen, sizeof seen);

	TickType_t now = xTaskGetTickCount();
	int n = 0;
	bool used[BRIDGE_MAX_CLIENT_IPS] = { false };
	while (n < max) {
		int newest = -1;
		for (int i = 0; i < count; i++) {
			if (!used[i] && (newest < 0 || now - seen[i] < now - seen[newest])) {
				newest = i;
			}
		}
		if (newest < 0 || (n > 0 && now - seen[newest] > pdMS_TO_TICKS(CLIENT_IP_RECENT_MS))) {
			break;
		}
		used[newest] = true;
		ips[n++] = ip[newest];
	}
	return n;
}

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
		learn_client_ip(frame, len);
		watch_dhcp_request(frame, len);
		if (stand_in_arp(frame, len)) {
			/* no Wi-Fi since start: we stood in for its gateway */
		} else if (svcnet_input(frame, len)) {
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
	watch_dhcp_reply(frame, len);
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
	ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event, NULL));
}

/* bridge mode: every frame, whatever its destination MAC */
static void ethernet_start_bridge(void)
{
	bool promiscuous = true;
	ESP_ERROR_CHECK(esp_eth_update_input_path(s_eth, eth_rx, NULL));
	ESP_ERROR_CHECK(esp_eth_ioctl(s_eth, ETH_CMD_S_PROMISCUOUS, &promiscuous));
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
		ethernet_start_bridge();
	} else {
		ESP_LOGI(TAG, "no saved client, waiting for the first frame on Ethernet");
		ethernet_init();
		ethernet_start_bridge();
		xEventGroupWaitBits(s_events, CLIENT_LEARNED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
		memcpy(mac, (const uint8_t *) s_status.client_mac, 6);
		ESP_LOGI(TAG, "learned client " MACSTR_FMT, MAC2ARGS(mac));
		eventlog_add("learned client %02x%02x%02x%02x%02x%02x", MAC2ARGS(mac));
		settings_set_client_mac(mac);
	}

	svcnet_start(mac);
	wifi_sta_start(mac, wifi_rx, wifi_connected);
}

/* Setup mode: WRG frames go to the config protocol, everything else to lwIP */
static esp_err_t setup_eth_rx(esp_eth_handle_t eth, uint8_t *frame, uint32_t len, void *priv, void *info)
{
	if (is_config_frame(frame, len)) {
		rawcfg_input(frame, len);
		free(frame);
		return ESP_OK;
	}
	return esp_netif_receive(priv, frame, len, NULL);	/* takes the frame */
}

void bridge_start_setup(void)
{
	esp_netif_ip_info_t ip_info;
	if (esp_netif_str_to_ip4(CONFIG_WRG_SERVICE_IP, &ip_info.ip) != ESP_OK) {
		ESP_LOGE(TAG, "invalid service address \"%s\"", CONFIG_WRG_SERVICE_IP);
		return;
	}
	ip_info.gw = ip_info.ip;	/* the retro machine's gateway is us */
	esp_netif_set_ip4_addr(&ip_info.netmask, 255, 255, 255, 0);

	/* our own address on the wire, with a DHCP server: Telnet works without
	 * any settings on the retro machine. Only in setup mode, where nothing
	 * is bridged, so no other DHCP server can be confused. */
	esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
	base.flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP;
	base.ip_info = &ip_info;
	base.get_ip_event = 0;
	base.lost_ip_event = 0;
	base.if_key = "ETH_SETUP";
	base.route_prio = 5;
	esp_netif_config_t config = { .base = &base, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH };
	esp_netif_t *netif = esp_netif_new(&config);

	/* 2 hours: mTCP refuses to start with less than an hour left. After
	 * setup the user runs DHCP again (the reboot message says so). */
	uint32_t lease_minutes = 120;
	esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_IP_ADDRESS_LEASE_TIME,
			       &lease_minutes, sizeof lease_minutes);

	ethernet_init();
	ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(s_eth)));
	/* replaces the glue's input path (it registers the _info variant too) */
	ESP_ERROR_CHECK(esp_eth_update_input_path_info(s_eth, setup_eth_rx, netif));
	ESP_ERROR_CHECK(esp_eth_start(s_eth));
	ESP_LOGI(TAG, "setup: Ethernet on %s with DHCP", CONFIG_WRG_SERVICE_IP);
	eventlog_add("setup: Ethernet %s, DHCP", CONFIG_WRG_SERVICE_IP);
}

void bridge_get_status(bridge_status_t *status)
{
	memcpy(status, (const void *) &s_status, sizeof *status);

	TickType_t last = s_last_ipx_ethernet_ii;
	TickType_t now = xTaskGetTickCount();
	status->ipx_ethernet_ii = last != 0 && now - last < pdMS_TO_TICKS(60000);

	TickType_t asks_seen = s_asks_seen;
	status->asks_known = asks_seen != 0 && now - asks_seen < pdMS_TO_TICKS(10 * 60 * 1000);
	status->asks_ip = s_asks_ip;
	status->asks_age_s = (now - asks_seen) / configTICK_RATE_HZ;

	/* still asking (a request within the last minute), unanswered for 8 s */
	TickType_t since = s_dhcp_waiting_since, last_request = s_dhcp_last_request;
	status->dhcp_waiting_s = since ? (now - since) / configTICK_RATE_HZ : 0;
	status->dhcp_no_answer = since != 0 && now - since >= pdMS_TO_TICKS(8000)
				 && now - last_request < pdMS_TO_TICKS(60000);

	int stand_in = s_stand_in_count;
	status->gateway_stand_in = !wifi_sta_up && wifi_sta_unreachable_since_boot();
	status->stand_in_count = stand_in;
	status->stand_in_ip = stand_in > 0 ? s_stand_in[stand_in - 1].target : 0;
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
