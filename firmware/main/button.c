/*
 * Setup button. See button.h.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "button.h"
#include "display.h"

static const char *TAG = "button";

#define POLL_MS		20
#define LONG_PRESS_MS	2000

static int s_gpio;
static button_handler_t *s_long_press;

static void button_task(void *arg)
{
	int pressed_ms = 0;

	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(POLL_MS));

		if (gpio_get_level(s_gpio) == 0) {
			if (pressed_ms == 0) {
				display_wake();
			}
			pressed_ms += POLL_MS;
			if (pressed_ms == LONG_PRESS_MS) {
				ESP_LOGI(TAG, "long press");
				s_long_press();
			}
		} else {
			pressed_ms = 0;
		}
	}
}

void button_start(int gpio, button_handler_t *long_press)
{
	if (gpio < 0) {
		return;
	}
	s_gpio = gpio;
	s_long_press = long_press;

	gpio_config_t config = {
		.pin_bit_mask = 1ULL << gpio,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
	};
	gpio_config(&config);

	xTaskCreate(button_task, "button", 2048, NULL, 3, NULL);
}
