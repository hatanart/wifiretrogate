/*
 * Telnet console. See telnet.h.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "console.h"
#include "telnet.h"

static const char *TAG = "telnet";

#define TELNET_PORT	23
#define MAX_SESSIONS	2
#define INPUT_MAX	160

/* Telnet protocol bytes */
#define IAC		255
#define SB		250
#define SE		240
#define WILL		251	/* WILL, WONT, DO, DONT: 251..254 */

static volatile int s_sessions;

static void telnet_write(void *ctx, const char *text)
{
	int sock = *(int *) ctx;
	const char *p = text;

	/* NVT wants CR LF */
	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t n = nl ? (size_t) (nl - p) : strlen(p);
		if (n > 0) {
			send(sock, p, n, 0);
		}
		if (nl) {
			send(sock, "\r\n", 2, 0);
			n++;
		}
		p += n;
	}
}

static void session_task(void *arg)
{
	int sock = (int) arg;
	char line[INPUT_MAX];
	size_t len = 0;
	enum { DATA, COMMAND, OPTION, SUBNEG, SUBNEG_IAC } state = DATA;
	bool last_cr = false;
	uint8_t buffer[64];

	telnet_write(&sock, "WifiRetroGate console, type help (quit to leave)\nwrg> ");

	for (;;) {
		int n = recv(sock, buffer, sizeof buffer, 0);
		if (n <= 0) {
			break;
		}
		for (int i = 0; i < n; i++) {
			uint8_t c = buffer[i];

			/* strip Telnet commands and option negotiation */
			switch (state) {
			case COMMAND:
				state = c == SB ? SUBNEG : c == IAC ? DATA : c >= WILL ? OPTION : DATA;
				if (c != IAC) {
					continue;
				}
				break;		/* IAC IAC: a literal 255 */
			case OPTION:
				state = DATA;
				continue;
			case SUBNEG:
				if (c == IAC) {
					state = SUBNEG_IAC;
				}
				continue;
			case SUBNEG_IAC:
				state = c == SE ? DATA : SUBNEG;
				continue;
			case DATA:
				if (c == IAC) {
					state = COMMAND;
					continue;
				}
				break;
			}

			if (c == '\r' || (c == '\n' && !last_cr)) {
				line[len] = '\0';
				len = 0;
				if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
					goto done;
				}
				console_exec(line, telnet_write, &sock);
				telnet_write(&sock, "wrg> ");
			} else if (c == 0x08 || c == 0x7f) {
				if (len > 0) {
					len--;
				}
			} else if (c >= 0x20 && c != IAC && len < INPUT_MAX - 1) {
				line[len++] = c;
			}
			last_cr = c == '\r';
		}
	}

done:
	close(sock);
	s_sessions--;
	vTaskDelete(NULL);
}

static void listen_task(void *arg)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(TELNET_PORT),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	int yes = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
	if (listener < 0 || bind(listener, (struct sockaddr *) &addr, sizeof addr) < 0
	    || listen(listener, 1) < 0) {
		ESP_LOGE(TAG, "cannot listen on port %d", TELNET_PORT);
		vTaskDelete(NULL);
	}

	for (;;) {
		int sock = accept(listener, NULL, NULL);
		if (sock < 0) {
			continue;
		}
		if (s_sessions >= MAX_SESSIONS) {
			const char *busy = "WifiRetroGate console busy\r\n";
			send(sock, busy, strlen(busy), 0);
			close(sock);
			continue;
		}
		/* notice dead clients (cable pulled, machine switched off) */
		setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof yes);
		s_sessions++;
		xTaskCreate(session_task, "telnet", 4096, (void *) sock, 3, NULL);
	}
}

void telnet_start(void)
{
	xTaskCreate(listen_task, "telnetd", 3072, NULL, 3, NULL);
}
