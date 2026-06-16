#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "epaper_driver_bsp.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "driver";

// Expand 4 bits of 1bpp data into one 2bpp byte.
// Input nibble: bit3=leftmost pixel, bit0=rightmost.
// Output: each pixel becomes 00 (BLACK) or 01 (WHITE) in 2bpp MSB-first.
// Lookup table computed offline for all 16 nibble values.
static const uint8_t EXPAND_NIBBLE[16] = {
    0x00, 0x01, 0x04, 0x05,
    0x10, 0x11, 0x14, 0x15,
    0x40, 0x41, 0x44, 0x45,
    0x50, 0x51, 0x54, 0x55,
};

epaper_driver_display::epaper_driver_display(int width, int height, custom_lcd_spi_t _lcd_spi_data) :
    lcd_spi_data(_lcd_spi_data),
    Width(width),
    Height(height)
{
    ESP_LOGI(TAG, "Initialize SPI");
    spi_port_init();
    spi_gpio_init();

    // Buffer is 1bpp — draw.cpp addresses it as (Width/8)*Height bytes.
    // EPD_Display() converts to 2bpp on-the-fly when writing to the panel.
    const int required_len = ((Width + 7) / 8) * Height;
    const int alloc_len = (lcd_spi_data.buffer_len < required_len) ? required_len : lcd_spi_data.buffer_len;

    ESP_LOGI(TAG, "Allocating %d-byte 1bpp display buffer", alloc_len);

    buffer = (uint8_t *)heap_caps_malloc(alloc_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        ESP_LOGW(TAG, "PSRAM alloc failed, trying internal RAM");
        buffer = (uint8_t *)heap_caps_malloc(alloc_len, MALLOC_CAP_8BIT);
    }

    assert(buffer);
    memset(buffer, 0xFF, alloc_len);  // start white

    bufferLen = alloc_len;

    // Snapshot for dirty-check in EPD_Display(). Inited to all-black so the
    // first display after boot always fires regardless of buffer content.
    lastBuffer = (uint8_t *)heap_caps_malloc(alloc_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lastBuffer)
        lastBuffer = (uint8_t *)heap_caps_malloc(alloc_len, MALLOC_CAP_8BIT);
    if (lastBuffer) memset(lastBuffer, 0x00, alloc_len);
}

epaper_driver_display::~epaper_driver_display() {}

void epaper_driver_display::spi_gpio_init()
{
    int rst  = lcd_spi_data.rst;
    int cs   = lcd_spi_data.cs;
    int dc   = lcd_spi_data.dc;
    int busy = lcd_spi_data.busy;

    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type    = GPIO_INTR_DISABLE;
    gpio_conf.mode         = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask = (0x1ULL << rst) | (0x1ULL << dc) | (0x1ULL << cs);
    gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en   = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

    gpio_conf.mode         = GPIO_MODE_INPUT;
    gpio_conf.pin_bit_mask = (0x1ULL << busy);
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

    set_rst_1();
}

void epaper_driver_display::spi_port_init()
{
    int mosi     = lcd_spi_data.mosi;
    int scl      = lcd_spi_data.scl;
    int spi_host = lcd_spi_data.spi_host;

    // 2bpp output is Width/4 * Height = 10000 bytes for 200x200
    const int max_tx = (Width / 4) * Height;

    spi_bus_config_t buscfg = {};
    buscfg.miso_io_num    = -1;
    buscfg.mosi_io_num    = mosi;
    buscfg.sclk_io_num    = scl;
    buscfg.quadwp_io_num  = -1;
    buscfg.quadhd_io_num  = -1;
    buscfg.max_transfer_sz = max_tx;

    spi_device_interface_config_t devcfg = {};
    devcfg.spics_io_num   = -1;
    devcfg.clock_speed_hz = 4 * 1000 * 1000;  // 4MHz — conservative for G display
    devcfg.mode           = 0;
    devcfg.queue_size     = 7;

    ESP_ERROR_CHECK(spi_bus_initialize((spi_host_device_t)spi_host, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device((spi_host_device_t)spi_host, &devcfg, &spi));
}

// ─── Low-level SPI ────────────────────────────────────────────────────────────

void epaper_driver_display::SPI_SendByte(uint8_t data)
{
    spi_transaction_t t = {};
    t.length    = 8;
    t.tx_buffer = &data;
    ESP_ERROR_CHECK_WITHOUT_ABORT(spi_device_polling_transmit(spi, &t));
}

void epaper_driver_display::EPD_SendCommand(uint8_t command)
{
    set_dc_0(); set_cs_0();
    SPI_SendByte(command);
    set_cs_1();
}

void epaper_driver_display::EPD_SendData(uint8_t data)
{
    set_dc_1(); set_cs_0();
    SPI_SendByte(data);
    set_cs_1();
}

void epaper_driver_display::writeBytes(uint8_t *buf, int len)
{
    set_dc_1(); set_cs_0();
    spi_transaction_t t = {};
    t.length    = 8 * len;
    t.tx_buffer = buf;
    ESP_ERROR_CHECK_WITHOUT_ABORT(spi_device_polling_transmit(spi, &t));
    set_cs_1();
}

void epaper_driver_display::writeBytes(const uint8_t *buf, int len)
{
    writeBytes((uint8_t *)buf, len);
}

// ─── Busy waits ───────────────────────────────────────────────────────────────

// Wait until BUSY goes HIGH — used after power-on (0x04) and after refresh (0x12).
// 100ms initial delay matches the official Waveshare 1.54G driver timing.
void epaper_driver_display::read_busy_H()
{
    vTaskDelay(pdMS_TO_TICKS(100));
    int busy = lcd_spi_data.busy;
    while (gpio_get_level((gpio_num_t)busy) == 0) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ─── Init — 1.54G sequence (from waveshareteam/ESP32-S3-ePaper-1.54G) ────────

void epaper_driver_display::EPD_Init()
{
    // Hardware reset
    set_rst_1(); vTaskDelay(pdMS_TO_TICKS(200));
    set_rst_0(); vTaskDelay(pdMS_TO_TICKS(2));
    set_rst_1(); vTaskDelay(pdMS_TO_TICKS(200));

    EPD_SendCommand(0x4D); EPD_SendData(0x78);  // vendor init

    EPD_SendCommand(0x00);  // PSR: panel setting
    EPD_SendData(0x0F); EPD_SendData(0x29);

    EPD_SendCommand(0x06);  // BTST_P: booster soft start (1.54G specific)
    EPD_SendData(0x0D); EPD_SendData(0x12); EPD_SendData(0x30);
    EPD_SendData(0x20); EPD_SendData(0x19); EPD_SendData(0x2A); EPD_SendData(0x22);

    EPD_SendCommand(0x50); EPD_SendData(0x37);  // CDI

    EPD_SendCommand(0x61);  // TRES: resolution
    EPD_SendData(Width  >> 8); EPD_SendData(Width  & 0xFF);
    EPD_SendData(Height >> 8); EPD_SendData(Height & 0xFF);

    EPD_SendCommand(0xE9); EPD_SendData(0x01);
    EPD_SendCommand(0x30); EPD_SendData(0x08);

    EPD_SendCommand(0x04);  // PON: power on
    read_busy_H();           // wait until BUSY goes HIGH
}

// ─── Fast init — same as EPD_Init but activates fast-refresh LUT ─────────────
// Call once after the first EPD_Display(). All subsequent Display() calls will
// then use the fast LUT (~3-5s refresh instead of ~14s), at slight quality cost.

void epaper_driver_display::EPD_Init_Fast()
{
    set_rst_1(); vTaskDelay(pdMS_TO_TICKS(200));
    set_rst_0(); vTaskDelay(pdMS_TO_TICKS(2));
    set_rst_1(); vTaskDelay(pdMS_TO_TICKS(200));

    EPD_SendCommand(0x4D); EPD_SendData(0x78);

    EPD_SendCommand(0x00);
    EPD_SendData(0x0F); EPD_SendData(0x29);

    EPD_SendCommand(0x06);
    EPD_SendData(0x0D); EPD_SendData(0x12); EPD_SendData(0x30);
    EPD_SendData(0x20); EPD_SendData(0x19); EPD_SendData(0x2A); EPD_SendData(0x22);

    EPD_SendCommand(0x50); EPD_SendData(0x37);

    EPD_SendCommand(0x61);
    EPD_SendData(Width  >> 8); EPD_SendData(Width  & 0xFF);
    EPD_SendData(Height >> 8); EPD_SendData(Height & 0xFF);

    EPD_SendCommand(0xE9); EPD_SendData(0x01);
    EPD_SendCommand(0x30); EPD_SendData(0x08);

    EPD_SendCommand(0x04);
    read_busy_H();

    // Fast-mode LUT activation (the three commands that make it fast)
    EPD_SendCommand(0xE0); EPD_SendData(0x02);
    EPD_SendCommand(0xE6); EPD_SendData(0x5D);
    EPD_SendCommand(0xA5); EPD_SendData(0x00);
    read_busy_H();
}

// ─── Clear — fill 1bpp buffer with white ─────────────────────────────────────

void epaper_driver_display::EPD_Clear()
{
    int len = ((Width + 7) / 8) * Height;
    memset(buffer, 0xFF, len);
}

// ─── Display — convert 1bpp buffer to 2bpp and send to panel ─────────────────
//
// 1bpp:  each bit = 1 pixel, 1=white, 0=black, 25 bytes/row
// 2bpp:  each 2-bit pair = 1 pixel, 01=white, 00=black, 50 bytes/row
// Each 1bpp byte → two 2bpp bytes via EXPAND_NIBBLE lookup on each nibble.

void epaper_driver_display::EPD_Display()
{
    const int bpr_1bpp = (Width + 7) / 8;       // 25 bytes/row
    const int total_1bpp = bpr_1bpp * Height;    // 5000 bytes
    const int total_2bpp = (Width / 4) * Height; // 10000 bytes

    // Skip the refresh if the buffer hasn't changed since last display.
    if (lastBuffer && memcmp(buffer, lastBuffer, total_1bpp) == 0) {
        ESP_LOGI(TAG, "EPD_Display: buffer unchanged — skipping refresh");
        return;
    }

    // Wake panel from post-refresh power-off state.
    // Register config is retained after POF so a simple PON is enough.
    EPD_SendCommand(0x04);  // PON: power on
    read_busy_H();

    uint8_t *buf2 = (uint8_t *)heap_caps_malloc(total_2bpp, MALLOC_CAP_8BIT);

    if (buf2) {
        for (int i = 0; i < total_1bpp; i++) {
            uint8_t b = buffer[i];
            buf2[i * 2]     = EXPAND_NIBBLE[(b >> 4) & 0xF];
            buf2[i * 2 + 1] = EXPAND_NIBBLE[b & 0xF];
        }
        EPD_SendCommand(0x10);
        writeBytes(buf2, total_2bpp);
        heap_caps_free(buf2);
    } else {
        // Low-memory fallback: send byte-by-byte
        EPD_SendCommand(0x10);
        set_dc_1(); set_cs_0();
        for (int i = 0; i < total_1bpp; i++) {
            uint8_t b = buffer[i];
            SPI_SendByte(EXPAND_NIBBLE[(b >> 4) & 0xF]);
            SPI_SendByte(EXPAND_NIBBLE[b & 0xF]);
        }
        set_cs_1();
    }

    EPD_SendCommand(0x12); EPD_SendData(0x00);  // DRF: display refresh
    read_busy_H();

    // Power off after every refresh. Leaving the panel drivers active long-term
    // permanently damages the internal layer (per Waveshare 1.54G datasheet).
    // EPD_Init_Fast() at the top of the next EPD_Display() wakes it again.
    EPD_SendCommand(0x02);  // POF: power off
    read_busy_H();

    // Snapshot the buffer so the next EPD_Display() can skip if unchanged.
    if (lastBuffer) memcpy(lastBuffer, buffer, total_1bpp);
}

// ─── Single pixel draw ────────────────────────────────────────────────────────

void epaper_driver_display::EPD_DrawColorPixel(uint16_t x, uint16_t y, uint8_t color)
{
    if (x >= (uint16_t)Width || y >= (uint16_t)Height) return;

    const uint16_t bpr = (Width + 7) / 8;
    uint32_t index = (uint32_t)y * bpr + (x >> 3);

    if (index >= (uint32_t)lcd_spi_data.buffer_len) return;

    uint8_t bit = 7 - (x & 0x07);
    if (color == DRIVER_COLOR_WHITE)
        buffer[index] |=  (0x01 << bit);
    else
        buffer[index] &= ~(0x01 << bit);
}

