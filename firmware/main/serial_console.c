/*
 * Serial console on UART0 (the flashing/log port, 115200 8N1).
 * Works with dumb terminals: plain line input with backspace, no escape codes.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "console.h"
#include "serial_console.h"

#define UART		UART_NUM_0
#define INPUT_MAX	160

static void serial_write(void *ctx, const char *text)
{
	/* terminals want CR LF */
	const char *p = text;
	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t n = nl ? (size_t) (nl - p) : strlen(p);
		uart_write_bytes(UART, p, n);
		if (nl) {
			uart_write_bytes(UART, "\r\n", 2);
			n++;
		}
		p += n;
	}
}

static void serial_task(void *arg)
{
	char line[INPUT_MAX];
	size_t len = 0;
	bool escape = false;

	serial_write(NULL, "\nWifiRetroGate console, type help\nwrg> ");

	for (;;) {
		uint8_t c;
		if (uart_read_bytes(UART, &c, 1, portMAX_DELAY) != 1) {
			continue;
		}

		if (escape) {
			/* skip ANSI sequences such as arrow keys: ESC [ ... letter */
			if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~') {
				escape = false;
			}
		} else if (c == 0x1b) {
			escape = true;
		} else if (c == '\r' || c == '\n') {
			if (c == '\n' && len == 0) {
				continue;	/* LF after CR */
			}
			serial_write(NULL, "\n");
			line[len] = '\0';
			console_exec(line, serial_write, NULL);
			len = 0;
			serial_write(NULL, "wrg> ");
		} else if (c == 0x08 || c == 0x7f) {
			if (len > 0) {
				len--;
				serial_write(NULL, "\b \b");
			}
		} else if (c >= 0x20 && len < INPUT_MAX - 1) {
			line[len++] = c;
			uart_write_bytes(UART, &c, 1);	/* echo */
		}
	}
}

void serial_console_start(void)
{
	uart_driver_install(UART, 256, 1024, 0, NULL, 0);
	/* route printf/log output through the driver too, so they do not collide */
	uart_vfs_dev_use_driver(UART);

	xTaskCreate(serial_task, "serial", 4096, NULL, 3, NULL);
}
