/*
 * Lightweight text-only driver for 128x64 / 128x32 I2C OLEDs
 * (SSD1306 and SH1106 controllers).
 *
 * 21 columns x 8 (or 4) rows with a 5x8 font. Text is kept in a line buffer
 * and rendered on oled_update(), which only sends rows that changed.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define OLED_COLS	21

typedef enum {
	OLED_SSD1306,
	OLED_SH1106,		/* 132-column RAM, image needs a 2-column offset */
} oled_type_t;

typedef struct {
	int sda_gpio;
	int scl_gpio;
	uint8_t address;	/* 0x3c or 0x3d */
	oled_type_t type;
	int height;		/* 64 or 32 */
	bool rotate180;
	uint8_t contrast;	/* 0..255; low values reduce burn-in */
} oled_config_t;

/* Returns an error if no display answers; the other calls are then no-ops. */
esp_err_t oled_init(const oled_config_t *config);

int oled_rows(void);

/* true if oled_init() found a display */
bool oled_present(void);

/* Set a whole row (UTF-8 text, Latin-1 characters supported, rest shown as '?').
 * The row is padded with spaces; text beyond 21 characters is cut off. */
void oled_print(int row, const char *text);
void oled_printf(int row, const char *format, ...) __attribute__((format(printf, 2, 3)));
void oled_clear(void);

/* Send changed rows to the display. */
void oled_update(void);

/* Burn-in protection */
void oled_power(bool on);
void oled_set_contrast(uint8_t contrast);
void oled_set_shift(int pixels);	/* 0..2: move the text right by a few pixels */
