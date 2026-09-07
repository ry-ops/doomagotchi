#include "display.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

static const char *TAG = "disp";

// ---- Cardputer Adv ST7789 wiring ---------------------------------------
#define LCD_HOST   SPI3_HOST      // SD is on SPI2; keep the panel separate
#define PIN_SCLK   36
#define PIN_MOSI   35
#define PIN_DC     34
#define PIN_CS     37
#define PIN_RST    33
#define PIN_BL     38

#define LCD_W      240
#define LCD_H      135
#define LCD_PCLK_HZ (40 * 1000 * 1000)

// Orientation / offset. M5GFX's Cardputer profile is offset_x 52, offset_y 40,
// rotation 1 (portrait 135x240 controller shown as 240x135 landscape). With the
// esp_lcd st7789 driver that becomes swap_xy + a mirror + a gap. These four
// knobs are the whole hardware-tuning surface -- adjust once on glass.
#define ORIENT_SWAP_XY   true
#define ORIENT_MIRROR_X  true
#define ORIENT_MIRROR_Y  false
#define GAP_X            40
#define GAP_Y            52
#define INVERT_COLOR     true

// ---- RGB565 helpers ---------------------------------------------------
// The ST7789 takes pixel data big-endian; the S3 writes the framebuffer
// little-endian and esp_lcd's SPI path doesn't swap for us. So bake the byte
// swap into every colour constant -- the framebuffer then holds exactly what
// goes on the wire. (Symptom without this: tan renders yellow, amber renders
// lavender -- the low/high bytes traded.)
#define RGB(r, g, b) __builtin_bswap16((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b) >> 3)))
// pwnagotchi look: 1-bit, black ink on a white ground. 0x0000 / 0xFFFF are
// byte-order-invariant so the mono UI is immune to any RGB565 endian question.
#define C_PAPER   RGB(255, 255, 255)
#define C_INK     RGB(0, 0, 0)
#define C_DIM     RGB(128, 128, 128)

static uint16_t *s_fb;                  // LCD_W * LCD_H, RGB565, big-endian on wire
static esp_lcd_panel_handle_t s_panel;
static bool s_ready;

// ---------------------------------------------------------------------
// 5x7 font (classic glcdfont printable subset 0x20..0x7E), column-major,
// bit0 = top row. Public-domain Adafruit GFX glyph data.
// ---------------------------------------------------------------------
static const uint8_t FONT[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14}, {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00}, {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02}, {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00}, {0x00,0x08,0x14,0x22,0x41}, {0x14,0x14,0x14,0x14,0x14},
    {0x41,0x22,0x14,0x08,0x00}, {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40}, {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00},
    {0x7F,0x10,0x28,0x44,0x00}, {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, {0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C}, {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00}, {0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00}, {0x08,0x04,0x08,0x10,0x08},
};

// ---------------------------------------------------------------------
// framebuffer primitives
// ---------------------------------------------------------------------
static inline void px(int x, int y, uint16_t c)
{
    if ((unsigned)x < LCD_W && (unsigned)y < LCD_H) s_fb[y * LCD_W + x] = c;
}

static void fill_rect(int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    for (int j = 0; j < h; j++) {
        uint16_t *row = &s_fb[(y + j) * LCD_W + x];
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

static void clear(uint16_t c)
{
    for (int i = 0; i < LCD_W * LCD_H; i++) s_fb[i] = c;
}

// scale 1 => 5x7 glyphs advancing 6px; scale 2 => 10x14 advancing 12.
static int draw_char(int x, int y, char ch, uint16_t fg, int scale)
{
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    const uint8_t *g = FONT[ch - 0x20];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                if (scale == 1) px(x + col, y + row, fg);
                else fill_rect(x + col * scale, y + row * scale, scale, scale, fg);
            }
        }
    }
    return x + 6 * scale;
}

static int draw_text(int x, int y, const char *s, uint16_t fg, int scale)
{
    for (; *s; s++) {
        if (*s == ' ') { x += 6 * scale; continue; }
        x = draw_char(x, y, *s, fg, scale);
    }
    return x;
}

// word-wrap `s` into the box [x0,x1) starting at y0, line pitch `lh`.
static void draw_wrapped(int x0, int y0, int x1, int lh, const char *s, uint16_t fg)
{
    int maxchars = (x1 - x0) / 6;
    char line[64];
    int y = y0;
    while (*s && y < LCD_H - 7) {
        while (*s == ' ') s++;
        int n = 0, lastsp = -1;
        while (s[n] && n < maxchars && n < (int)sizeof(line) - 1) {
            if (s[n] == ' ') lastsp = n;
            n++;
        }
        if (s[n] && lastsp > 0) n = lastsp;    // break at last space that fit
        memcpy(line, s, n);
        line[n] = 0;
        draw_text(x0, y, line, fg, 1);
        s += n;
        y += lh;
    }
}

static int text_w(const char *s, int scale) { return (int)strlen(s) * 6 * scale; }
static void draw_text_right(int xr, int y, const char *s, uint16_t fg, int scale)
{
    draw_text(xr - text_w(s, scale), y, s, fg, scale);
}
static void draw_text_center(int y, const char *s, uint16_t fg, int scale)
{
    draw_text((LCD_W - text_w(s, scale)) / 2, y, s, fg, scale);
}

// ---------------------------------------------------------------------
// the face -- pwnagotchi-style kaomoji, one per mood. DOOMAGOTCHI is an
// *inverted* pwnagotchi: deadest-looking when the air is quiet, most alive
// mid-slaughter.
// ---------------------------------------------------------------------
static const char *const FACE[MOOD_COUNT] = {
    [MOOD_BORED]    = "(-_-)",
    [MOOD_RESTLESS] = "(~_~)",
    [MOOD_HUNTING]  = "(o_o)",
    [MOOD_MANIC]    = "(@_@)",
    [MOOD_RAMPAGE]  = "(x_x)",
};

static void draw_face(const mood_state_t *m)
{
    static int bt;
    bool blink = (++bt % 33) < 2 && m->mood != MOOD_BORED;

    char f[8];
    strncpy(f, FACE[m->mood], sizeof(f) - 1);
    f[sizeof(f) - 1] = 0;
    if (blink) {
        for (char *p = f; *p; p++)
            if (*p != '(' && *p != ')' && *p != '_') *p = '-';
    }
    draw_text_center(50, f, C_INK, 5);
}

// ---------------------------------------------------------------------
// public
// ---------------------------------------------------------------------
void display_render(const disp_model_t *m)
{
    if (!s_ready) return;

    clear(C_PAPER);

    char l[48];

    // --- top bar: channel + AP count | uptime ---
    snprintf(l, sizeof(l), "CH %u  APS %lu", m->channel, (unsigned long)m->aps);
    draw_text(3, 3, l, C_INK, 1);
    uint32_t up = m->uptime_s;
    snprintf(l, sizeof(l), "UP %lu:%02lu:%02lu",
             (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60),
             (unsigned long)(up % 60));
    draw_text_right(LCD_W - 3, 3, l, C_INK, 1);
    fill_rect(0, 13, LCD_W, 1, C_INK);

    // --- voice line: the mood quip, pwnagotchi's "speech" ---
    draw_wrapped(3, 19, LCD_W - 3, 10, m->mood.quip, C_INK);

    // --- big face ---
    draw_face(&m->mood);

    // --- corner readout (pwnagotchi's mem/cpu/temp slot) ---
    snprintf(l, sizeof(l), "HS %lu  PMK %lu",
             (unsigned long)m->handshakes, (unsigned long)m->pmkids);
    draw_text_right(LCD_W - 3, 96, l, C_INK, 1);
    snprintf(l, sizeof(l), "%s  i%u", m->mood.label, m->mood.intensity);
    draw_text_right(LCD_W - 3, 106, l, C_INK, 1);

    // --- bottom bar ---
    fill_rect(0, LCD_H - 14, LCD_W, 1, C_INK);
    snprintf(l, sizeof(l), "PWND %lu (%lu)",
             (unsigned long)(m->handshakes + m->pmkids), (unsigned long)m->aps);
    draw_text(3, LCD_H - 10, l, C_INK, 1);
    if (m->last_enemy) {
        snprintf(l, sizeof(l), "[%s]", m->last_enemy);
        draw_text_right(LCD_W - 3, LCD_H - 10, l, C_INK, 1);
    }

    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb);
}

bool display_start(void)
{
    if (s_ready) return true;

    s_fb = heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_fb) { ESP_LOGW(TAG, "no RAM for framebuffer - display off"); return false; }

    gpio_config_t bl = { .pin_bit_mask = 1ULL << PIN_BL, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&bl);
    gpio_set_level(PIN_BL, 0);   // off until the panel has valid content

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * LCD_H * sizeof(uint16_t) + 16,
    };
    esp_err_t e = spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "spi bus: %s", esp_err_to_name(e));
        free(s_fb); s_fb = NULL; return false;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,
        .pclk_hz = LCD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io) != ESP_OK) {
        ESP_LOGW(TAG, "panel io failed"); free(s_fb); s_fb = NULL; return false;
    }

    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &pcfg, &s_panel) != ESP_OK) {
        ESP_LOGW(TAG, "st7789 init failed"); free(s_fb); s_fb = NULL; return false;
    }

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, INVERT_COLOR);
    esp_lcd_panel_swap_xy(s_panel, ORIENT_SWAP_XY);
    esp_lcd_panel_mirror(s_panel, ORIENT_MIRROR_X, ORIENT_MIRROR_Y);
    esp_lcd_panel_set_gap(s_panel, GAP_X, GAP_Y);
    esp_lcd_panel_disp_on_off(s_panel, true);

    s_ready = true;

    // splash so a blank panel isn't mistaken for a dead one
    clear(C_PAPER);
    draw_text_center(30, "DOOMAGOTCHI", C_INK, 2);
    draw_text_center(58, "the airspace is the level", C_INK, 1);
    draw_text_center(80, "(o_o)", C_INK, 4);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb);
    gpio_set_level(PIN_BL, 1);

    ESP_LOGI(TAG, "ST7789 up (%dx%d)", LCD_W, LCD_H);
    return true;
}
