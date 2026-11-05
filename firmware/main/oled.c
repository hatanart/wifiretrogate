/*
 * Lightweight text-only driver for 128x64 / 128x32 I2C OLEDs
 * (SSD1306 and SH1106 controllers). See oled.h.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "oled.h"

static const char *TAG = "oled";

#define OLED_WIDTH	128
#define OLED_MAX_ROWS	8
#define GLYPH_WIDTH	5
#define CELL_WIDTH	6	/* glyph + 1 pixel gap: 21 * 6 = 126 pixels */
#define I2C_TIMEOUT_MS	50

/* I2C control bytes */
#define CTRL_COMMAND	0x00
#define CTRL_DATA	0x40

extern const uint8_t font5x8_ascii[95][5];	/* 32..126 */
extern const uint8_t font5x8_latin1[96][5];	/* 160..255 */

static i2c_master_dev_handle_t s_dev;
static bool s_present;
static oled_config_t s_config;
static int s_rows;
static int s_shift;

/* text per row as Latin-1 bytes, plus "needs sending" flags */
static uint8_t s_text[OLED_MAX_ROWS][OLED_COLS];
static bool s_dirty[OLED_MAX_ROWS];

static esp_err_t send_commands(const uint8_t *commands, size_t count)
{
	uint8_t buffer[1 + 32];

	if (count > sizeof buffer - 1) {
		return ESP_ERR_INVALID_SIZE;
	}
	buffer[0] = CTRL_COMMAND;
	memcpy(buffer + 1, commands, count);
	return i2c_master_transmit(s_dev, buffer, count + 1, I2C_TIMEOUT_MS);
}

static esp_err_t send_command(uint8_t command)
{
	return send_commands(&command, 1);
}

static const uint8_t *glyph(uint8_t c)
{
	if (c >= 32 && c <= 126) {
		return font5x8_ascii[c - 32];
	}
	if (c >= 160) {
		return font5x8_latin1[c - 160];
	}
	return font5x8_ascii['?' - 32];
}

/* Render one text row into a page (8 pixel rows) and send it. */
static void send_row(int row)
{
	uint8_t buffer[1 + OLED_WIDTH] = { CTRL_DATA };
	uint8_t *pixels = buffer + 1;

	for (int col = 0; col < OLED_COLS; col++) {
		memcpy(pixels + s_shift + col * CELL_WIDTH, glyph(s_text[row][col]), GLYPH_WIDTH);
	}

	/* page addressing: works the same on SSD1306 and SH1106 */
	unsigned column = s_config.type == OLED_SH1106 ? 2 : 0;
	uint8_t address[] = {
		0xb0 | row,			/* page */
		0x00 | (column & 0x0f),		/* column, low nibble */
		0x10 | (column >> 4),		/* column, high nibble */
	};
	if (send_commands(address, sizeof address) == ESP_OK) {
		i2c_master_transmit(s_dev, buffer, sizeof buffer, I2C_TIMEOUT_MS);
	}
}

esp_err_t oled_init(const oled_config_t *config)
{
	s_config = *config;
	s_rows = config->height == 32 ? 4 : 8;

	i2c_master_bus_config_t bus_config = {
		.i2c_port = -1,
		.sda_io_num = config->sda_gpio,
		.scl_io_num = config->scl_gpio,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	i2c_master_bus_handle_t bus;
	esp_err_t err = i2c_new_master_bus(&bus_config, &bus);
	if (err != ESP_OK) {
		return err;
	}

	err = i2c_master_probe(bus, config->address, I2C_TIMEOUT_MS);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "no display at 0x%02x (SDA %d, SCL %d)",
			 config->address, config->sda_gpio, config->scl_gpio);
		return err;
	}

	i2c_device_config_t device_config = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = config->address,
		.scl_speed_hz = 400000,
	};
	err = i2c_master_bus_add_device(bus, &device_config, &s_dev);
	if (err != ESP_OK) {
		return err;
	}

	const uint8_t common[] = {
		0xae,				/* display off */
		0xd5, 0x80,			/* clock divider */
		0xa8, s_rows * 8 - 1,		/* multiplex ratio = height - 1 */
		0xd3, 0x00,			/* display offset */
		0x40,				/* start line 0 */
		config->rotate180 ? 0xa0 : 0xa1,	/* segment remap */
		config->rotate180 ? 0xc0 : 0xc8,	/* COM scan direction */
		0xda, config->height == 32 ? 0x02 : 0x12,	/* COM pins */
		0x81, config->contrast,		/* contrast */
		0xd9, 0xf1,			/* pre-charge period */
		0xdb, 0x40,			/* VCOMH level */
		0xa4,				/* show RAM contents */
		0xa6,				/* normal (not inverted) */
	};
	err = send_commands(common, sizeof common);
	if (err == ESP_OK) {
		if (config->type == OLED_SH1106) {
			const uint8_t dc_dc_on[] = { 0xad, 0x8b };
			err = send_commands(dc_dc_on, sizeof dc_dc_on);
		} else {
			const uint8_t charge_pump_on[] = { 0x8d, 0x14, 0x20, 0x02 };	/* + page addressing */
			err = send_commands(charge_pump_on, sizeof charge_pump_on);
		}
	}
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "display init failed: %s", esp_err_to_name(err));
		return err;
	}

	s_present = true;
	oled_clear();
	oled_update();
	send_command(0xaf);		/* display on */

	ESP_LOGI(TAG, "%s %dx%d at 0x%02x", config->type == OLED_SH1106 ? "SH1106" : "SSD1306",
		 OLED_WIDTH, config->height, config->address);
	return ESP_OK;
}

int oled_rows(void)
{
	return s_rows;
}

/* Decode UTF-8 into Latin-1; anything outside Latin-1 becomes '?'. */
void oled_print(int row, const char *text)
{
	uint8_t line[OLED_COLS];
	const uint8_t *p = (const uint8_t *) text;
	int col = 0;

	if (row < 0 || row >= s_rows) {
		return;
	}

	while (*p && col < OLED_COLS) {
		unsigned c = *p++;
		if (c >= 0x80) {
			unsigned extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
			c &= 0x3f >> extra;
			for (; extra && (*p & 0xc0) == 0x80; extra--) {
				c = (c << 6) | (*p++ & 0x3f);
			}
			if (c < 0xa0 || c > 0xff) {
				c = '?';
			}
		}
		line[col++] = c;
	}
	memset(line + col, ' ', OLED_COLS - col);

	if (memcmp(s_text[row], line, OLED_COLS) != 0) {
		memcpy(s_text[row], line, OLED_COLS);
		s_dirty[row] = true;
	}
}

void oled_printf(int row, const char *format, ...)
{
	char text[64];
	va_list args;

	va_start(args, format);
	vsnprintf(text, sizeof text, format, args);
	va_end(args);
	oled_print(row, text);
}

void oled_clear(void)
{
	for (int row = 0; row < s_rows; row++) {
		memset(s_text[row], ' ', OLED_COLS);
		s_dirty[row] = true;
	}
}

bool oled_present(void)
{
	return s_present;
}

void oled_update(void)
{
	if (!s_present) {
		return;
	}
	for (int row = 0; row < s_rows; row++) {
		if (s_dirty[row]) {
			send_row(row);
			s_dirty[row] = false;
		}
	}
}

void oled_power(bool on)
{
	if (s_present) {
		send_command(on ? 0xaf : 0xae);
	}
}

void oled_set_contrast(uint8_t contrast)
{
	if (s_present) {
		const uint8_t command[] = { 0x81, contrast };
		send_commands(command, sizeof command);
	}
}

void oled_set_shift(int pixels)
{
	if (pixels < 0 || pixels > OLED_WIDTH - OLED_COLS * CELL_WIDTH) {
		pixels = 0;
	}
	if (pixels != s_shift) {
		s_shift = pixels;
		for (int row = 0; row < s_rows; row++) {
			s_dirty[row] = true;
		}
	}
}
