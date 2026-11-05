/*
 * Setup button: a push button between a GPIO and GND (internal pull-up).
 * Short press wakes the display, long press (2 s) calls the handler.
 */
#pragma once

typedef void button_handler_t(void);

/* gpio < 0: no button */
void button_start(int gpio, button_handler_t *long_press);
