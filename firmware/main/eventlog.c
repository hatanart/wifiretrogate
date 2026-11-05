/*
 * Event log. See eventlog.h.
 */
#include <stdarg.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "eventlog.h"

#define EVENTS		24
#define EVENT_TEXT	56

static char s_events[EVENTS][EVENT_TEXT];
static unsigned s_next;		/* total number of events added */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void eventlog_add(const char *format, ...)
{
	char text[EVENT_TEXT];
	unsigned seconds = esp_timer_get_time() / 1000000;
	int n = snprintf(text, sizeof text, "%3u:%02u:%02u ", seconds / 3600, seconds / 60 % 60, seconds % 60);

	va_list args;
	va_start(args, format);
	vsnprintf(text + n, sizeof text - n, format, args);
	va_end(args);

	taskENTER_CRITICAL(&s_lock);
	snprintf(s_events[s_next % EVENTS], EVENT_TEXT, "%s", text);
	s_next++;
	taskEXIT_CRITICAL(&s_lock);
}

void eventlog_dump(void (*write)(void *ctx, const char *line), void *ctx)
{
	unsigned first = s_next > EVENTS ? s_next - EVENTS : 0;

	for (unsigned i = first; i < s_next; i++) {
		char line[EVENT_TEXT];
		taskENTER_CRITICAL(&s_lock);
		snprintf(line, sizeof line, "%s", s_events[i % EVENTS]);
		taskEXIT_CRITICAL(&s_lock);
		write(ctx, line);
	}
}
