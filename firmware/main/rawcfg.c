/*
 * Raw-frame configuration protocol. See rawcfg.h.
 */
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "bridge.h"
#include "console.h"
#include "rawcfg.h"

static const char *TAG = "rawcfg";

#define HEADER		(14 + 9)
#define TYPE_REQUEST	1
#define TYPE_REPLY	2
#define FLAG_LAST	0x01
#define PART_TEXT	1400
#define OUTPUT_MAX	4096

typedef struct {
	uint32_t len;
	uint8_t frame[];
} request_t;

static QueueHandle_t s_queue;

typedef struct {
	char *text;
	size_t len;
} output_t;

static void collect(void *ctx, const char *text)
{
	output_t *output = ctx;
	size_t n = strlen(text);

	if (output->len + n >= OUTPUT_MAX) {
		n = OUTPUT_MAX - 1 - output->len;
	}
	memcpy(output->text + output->len, text, n);
	output->len += n;
}

static void send_reply(const uint8_t *request, const output_t *output)
{
	uint8_t frame[HEADER + PART_TEXT];
	size_t offset = 0;
	uint8_t part = 0;

	do {
		size_t n = output->len - offset;
		if (n > PART_TEXT) {
			n = PART_TEXT;
		}
		bool last = offset + n == output->len;

		memcpy(frame, request + 6, 6);		/* to the requester */
		bridge_eth_mac(frame + 6);		/* from us */
		frame[12] = WRG_ETHERTYPE >> 8;
		frame[13] = WRG_ETHERTYPE & 0xff;
		memcpy(frame + 14, "WRG", 3);
		frame[17] = 1;
		frame[18] = TYPE_REPLY;
		frame[19] = request[19];		/* sequence number */
		frame[20] = request[20];
		frame[21] = part++;
		frame[22] = last ? FLAG_LAST : 0;
		memcpy(frame + HEADER, output->text + offset, n);

		size_t len = HEADER + n;
		if (len < 60) {
			memset(frame + len, 0, 60 - len);	/* minimum Ethernet frame */
			len = 60;
		}
		bridge_eth_send(frame, len);
		offset += n;
	} while (offset < output->len);
}

static void handle_request(const uint8_t *frame, uint32_t len, output_t *output)
{
	if (len <= HEADER || memcmp(frame + 14, "WRG", 3) != 0
	    || frame[17] != 1 || frame[18] != TYPE_REQUEST) {
		return;
	}

	char line[200];
	size_t n = len - HEADER;
	if (n >= sizeof line) {
		n = sizeof line - 1;
	}
	memcpy(line, frame + HEADER, n);
	line[n] = '\0';		/* also cuts Ethernet padding */

	ESP_LOGI(TAG, "command from %02x:%02x:%02x:%02x:%02x:%02x: %s",
		 frame[6], frame[7], frame[8], frame[9], frame[10], frame[11], line);
	output->len = 0;
	console_exec(line, collect, output);
	send_reply(frame, output);
}

static void rawcfg_task(void *arg)
{
	output_t output = { .text = malloc(OUTPUT_MAX) };

	for (;;) {
		request_t *request;
		xQueueReceive(s_queue, &request, portMAX_DELAY);
		handle_request(request->frame, request->len, &output);
		free(request);
	}
}

void rawcfg_input(const uint8_t *frame, uint32_t len)
{
	if (s_queue == NULL) {
		s_queue = xQueueCreate(4, sizeof(request_t *));
		xTaskCreate(rawcfg_task, "rawcfg", 4096, NULL, 3, NULL);
	}

	request_t *request = malloc(sizeof *request + len);
	if (request == NULL) {
		return;
	}
	request->len = len;
	memcpy(request->frame, frame, len);
	if (xQueueSend(s_queue, &request, 0) != pdTRUE) {
		free(request);		/* busy: the tool will retry */
	}
}
