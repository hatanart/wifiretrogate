/*
 * Small in-memory log of notable events (Wi-Fi drops, link changes, client
 * changes) for the "log" console command.
 */
#pragma once

void eventlog_add(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* Call write() for each stored line, oldest first */
void eventlog_dump(void (*write)(void *ctx, const char *line), void *ctx);
