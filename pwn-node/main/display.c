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
#define RGB(r, g, b) ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)))
#define C_BG      RGB(12, 12, 18)
#define C_PANEL   RGB(28, 26, 34)
#define C_INK     RGB(210, 205, 200)
#define C_DIM     RGB(120, 118, 128)
#define C_RED     RGB(200, 30, 24)
#define C_DARKRED RGB(90, 12, 10)
#define C_AMBER   RGB(220, 150, 40)
#define C_GREEN   RGB(80, 200, 90)
#define C_SKIN    RGB(196, 150, 120)
#define C_SKINSH  RGB(140, 100, 80)
#define C_BLOOD   RGB(170, 20, 16)

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

static void frame_rect(int x, int y, int w, int h, uint16_t c)
{
    fill_rect(x, y, w, 1, c);
    fill_rect(x, y + h - 1, w, 1, c);
    fill_rect(x, y, 1, h, c);
    fill_rect(x + w - 1, y, 1, h, c);
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

// ---------------------------------------------------------------------
// the marine's face -- procedural, reacts to mood + intensity
// ---------------------------------------------------------------------
static uint32_t s_rng = 0x1234abcd;
static uint32_t xrand(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

static void draw_face(int fx, int fy, int fw, int fh, const mood_state_t *m)
{
    static int blink_t;
    bool blink = false;
    // higher intensity -> blinks more often (twitchy)
    int period = 40 - (int)(m->intensity / 8);
    if (period < 6) period = 6;
    if (++blink_t % period < 2) blink = true;

    fill_rect(fx, fy, fw, fh, C_PANEL);
    frame_rect(fx, fy, fw, fh, C_DIM);

    int hx = fx + 4, hy = fy + 4, hw = fw - 8, hh = fh - 8;
    fill_rect(hx, hy, hw, hh, C_SKIN);
    fill_rect(hx, hy + hh - 3, hw, 3, C_SKINSH);          // jaw shadow
    fill_rect(hx, hy, hw, 2, C_SKINSH);                    // brow shadow

    int eyeY = hy + hh / 3;
    int lx = hx + hw / 4 - 2, rx = hx + (3 * hw) / 4 - 2;
    uint16_t eyec = (m->mood >= MOOD_MANIC) ? C_RED : RGB(20, 20, 24);

    if (blink) {
        fill_rect(lx - 1, eyeY + 2, 6, 1, C_SKINSH);
        fill_rect(rx - 1, eyeY + 2, 6, 1, C_SKINSH);
    } else {
        fill_rect(lx, eyeY, 4, 3, eyec);
        fill_rect(rx, eyeY, 4, 3, eyec);
        if (m->mood >= MOOD_HUNTING) {                     // angry brows
            for (int i = 0; i < 6; i++) {
                px(lx - 1 + i, eyeY - 2 + i / 3, RGB(40, 30, 26));
                px(rx + 4 - i, eyeY - 2 + i / 3, RGB(40, 30, 26));
            }
        }
    }

    // mouth by mood
    int mY = hy + (2 * hh) / 3, mX = hx + hw / 4, mW = hw / 2;
    switch (m->mood) {
    case MOOD_BORED:
        fill_rect(mX, mY + 2, mW, 1, C_SKINSH);
        break;
    case MOOD_RESTLESS:
        for (int i = 0; i < mW; i++) px(mX + i, mY + 3 - (i < mW / 2 ? 0 : 1), C_SKINSH);
        break;
    case MOOD_HUNTING:
        fill_rect(mX, mY, mW, 3, RGB(30, 16, 16));
        break;
    case MOOD_MANIC:
        fill_rect(mX, mY, mW, 4, RGB(24, 12, 12));
        for (int i = 0; i < mW; i += 2) px(mX + i, mY, C_INK);    // teeth
        break;
    case MOOD_RAMPAGE:
    default:
        fill_rect(mX - 1, mY - 1, mW + 2, 6, RGB(20, 8, 8));
        for (int i = 0; i < mW + 2; i += 2) px(mX - 1 + i, mY - 1, C_INK);
        break;
    }

    // blood spatter scaled by intensity
    int spatter = m->intensity / 12;
    for (int i = 0; i < spatter; i++) {
        int bx = hx + (xrand() % hw);
        int by = hy + (xrand() % hh);
        px(bx, by, C_BLOOD);
        if (xrand() & 1) px(bx + 1, by, C_BLOOD);
    }
}

// ---------------------------------------------------------------------
// public
// ---------------------------------------------------------------------
static uint16_t mood_color(mood_t m)
{
    switch (m) {
    case MOOD_BORED:     return C_DIM;
    case MOOD_RESTLESS:  return C_INK;
    case MOOD_HUNTING:   return C_AMBER;
    case MOOD_MANIC:     return C_RED;
    case MOOD_RAMPAGE:   default: return RGB(255, 80, 40);
    }
}

void display_render(const disp_model_t *m)
{
    if (!s_ready) return;

    clear(C_BG);

    // title bar
    fill_rect(0, 0, LCD_W, 13, C_DARKRED);
    draw_text(LCD_W / 2 - (11 * 6) / 2, 3, "DOOMAGOTCHI", RGB(240, 200, 190), 1);

    // face
    draw_face(4, 17, 44, 44, &m->mood);

    // stat lines
    char buf[40];
    snprintf(buf, sizeof(buf), "CH:%02u   APS:%lu", m->channel, (unsigned long)m->aps);
    draw_text(56, 20, buf, C_INK, 1);
    snprintf(buf, sizeof(buf), "HS:%lu   PMKID:%lu",
             (unsigned long)m->handshakes, (unsigned long)m->pmkids);
    draw_text(56, 32, buf, C_INK, 1);
    draw_text(56, 44, "MOOD:", C_DIM, 1);
    draw_text(56 + 6 * 6, 44, m->mood.label, mood_color(m->mood.mood), 1);

    // intensity bar
    frame_rect(4, 66, LCD_W - 8, 9, C_DIM);
    int fillw = (LCD_W - 12) * m->mood.intensity / 255;
    fill_rect(6, 68, fillw, 5, mood_color(m->mood.mood));

    // quip
    draw_wrapped(4, 80, LCD_W - 4, 11, m->mood.quip, C_DIM);

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
    clear(C_BG);
    fill_rect(0, 0, LCD_W, 13, C_DARKRED);
    draw_text(LCD_W / 2 - (11 * 6) / 2, 3, "DOOMAGOTCHI", RGB(240, 200, 190), 1);
    draw_text(28, 60, "the airspace is the level", C_DIM, 1);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb);
    gpio_set_level(PIN_BL, 1);

    ESP_LOGI(TAG, "ST7789 up (%dx%d)", LCD_W, LCD_H);
    return true;
}
