/*
 * Command interpreter shared by the serial console, Telnet and the raw-frame
 * configuration tool. Output goes through a write function per transport.
 */
#pragma once

typedef void console_write_t(void *ctx, const char *text);

/* Run one command line. Output lines end with "\n". Calls from different
 * transports are serialized. */
void console_exec(const char *line, console_write_t *write, void *ctx);
