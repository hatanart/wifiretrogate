/*
 * Service address. See svcnet.h.
 */
#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "bridge.h"
#include "svcnet.h"

static const char *TAG = "svcnet";

#define ETH_HEADER	14
#define ETH_MTU		1500

static struct netif s_netif;
static bool s_started;
static uint32_t s_service_ip;		/* network byte order */
static uint8_t s_client_mac[6];
static uint8_t s_gateway_mac[6];	/* learned from the client's frames */

/* lwIP -> Ethernet: to the client, from the gateway's MAC */
static err_t svcnet_output(struct netif *netif, struct pbuf *p, const ip4_addr_t *ipaddr)
{
	/* only the lwIP thread calls this: a static buffer keeps it off its small stack */
	static uint8_t frame[ETH_HEADER + ETH_MTU];

	if (p->tot_len > ETH_MTU) {
		return ERR_BUF;
	}
	memcpy(frame, s_client_mac, 6);
	memcpy(frame + 6, s_gateway_mac, 6);
	frame[12] = 0x08;
	frame[13] = 0x00;
	pbuf_copy_partial(p, frame + ETH_HEADER, p->tot_len, 0);

	size_t len = ETH_HEADER + p->tot_len;
	if (len < 60) {
		memset(frame + len, 0, 60 - len);
		len = 60;
	}
	return bridge_eth_send(frame, len) == ESP_OK ? ERR_OK : ERR_IF;
}

static err_t svcnet_netif_init(struct netif *netif)
{
	netif->name[0] = 's';
	netif->name[1] = 'v';
	netif->mtu = ETH_MTU;
	netif->output = svcnet_output;
	netif->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;	/* no ARP, no broadcast */
	return ERR_OK;
}

static esp_err_t add_netif(void *ctx)
{
	ip4_addr_t ip, netmask, gw;

	ip.addr = s_service_ip;
	IP4_ADDR(&netmask, 255, 255, 255, 0);
	ip4_addr_set_zero(&gw);

	if (netif_add(&s_netif, &ip, &netmask, &gw, NULL, svcnet_netif_init, tcpip_input) == NULL) {
		return ESP_FAIL;
	}
	/* replies go to the client's own (LAN) address: route everything here */
	netif_set_default(&s_netif);
	netif_set_up(&s_netif);
	netif_set_link_up(&s_netif);
	return ESP_OK;
}

void svcnet_start(const uint8_t client_mac[6])
{
	ip4_addr_t ip;

	if (!ip4addr_aton(CONFIG_WRG_SERVICE_IP, &ip)) {
		ESP_LOGE(TAG, "invalid service address \"%s\"", CONFIG_WRG_SERVICE_IP);
		return;
	}
	s_service_ip = ip.addr;
	memcpy(s_client_mac, client_mac, 6);

	if (esp_netif_tcpip_exec(add_netif, NULL) == ESP_OK) {
		s_started = true;
		ESP_LOGI(TAG, "services on %s", CONFIG_WRG_SERVICE_IP);
	} else {
		ESP_LOGE(TAG, "cannot add service interface");
	}
}

bool svcnet_input(const uint8_t *frame, uint32_t len)
{
	if (!s_started || len < ETH_HEADER + 20 || frame[12] != 0x08 || frame[13] != 0x00
	    || (frame[ETH_HEADER] >> 4) != 4) {
		return false;
	}
	uint32_t dst;
	memcpy(&dst, frame + ETH_HEADER + 16, 4);
	if (dst != s_service_ip) {
		return false;
	}

	/* the destination MAC is the gateway the client sent this to */
	memcpy(s_gateway_mac, frame, 6);

	/* IP total length (without Ethernet padding) */
	uint32_t ip_len = frame[ETH_HEADER + 2] << 8 | frame[ETH_HEADER + 3];
	if (ip_len < 20 || ip_len > len - ETH_HEADER) {
		return true;		/* malformed: consume, do not forward */
	}

	struct pbuf *p = pbuf_alloc(PBUF_RAW, ip_len, PBUF_RAM);
	if (p == NULL) {
		return true;
	}
	memcpy(p->payload, frame + ETH_HEADER, ip_len);
	if (s_netif.input(p, &s_netif) != ERR_OK) {
		pbuf_free(p);
	}
	return true;
}
