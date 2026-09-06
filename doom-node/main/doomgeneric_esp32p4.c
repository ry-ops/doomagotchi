// doomgeneric_esp32p4.c — the DOOMAGOTCHI platform layer for the Tab5.
//
// The only file we own on the DOOM side (ADR 0001): the six DG_* functions.
// Everything above this line is unmodified upstream doomgeneric.
//
// Phase 0 status: timers real; DG_DrawFrame does HW rotate+scale to the
// MIPI-DSI panel via the PPA; DG_GetKey maps GT911 touch zones to DOOM keys
// (the "playable by hand" gate - Phase 1 replaces this with the autoplayer).

#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "doomgeneric.h"
#include "doomkeys.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_touch.h"
#include "driver/ppa.h"

#include "bsp/display.h"
#include "bsp/touch.h"
#include "bsp/m5stack_tab5.h"

static const char *TAG = "DG";

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;       // the DPI panel's real framebuffer, 720x1280 RGB565
static ppa_client_handle_t s_ppa;
static esp_lcd_touch_handle_t s_tp;

#define PANEL_W  720   // BSP_LCD_H_RES  (native portrait)
#define PANEL_H  1280  // BSP_LCD_V_RES

// DOOM keys we drive from touch, and their current/last-reported held state.
static const unsigned char DG_KEYS[] = {
    KEY_LEFTARROW, KEY_RIGHTARROW, KEY_UPARROW, KEY_DOWNARROW, KEY_FIRE, KEY_USE,
};
#define DG_NKEYS (sizeof(DG_KEYS) / sizeof(DG_KEYS[0]))
enum { ZL, ZR, ZU, ZD, ZFIRE, ZUSE };
static bool s_held[DG_NKEYS];
static bool s_reported[DG_NKEYS];

void DG_Init(void)
{
    bsp_display_config_t cfg = {
        .dsi_bus = {
            .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
            .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
        },
    };

    esp_lcd_panel_io_handle_t io = NULL;   // bsp_display_new dereferences ret_io unconditionally
    esp_err_t err = bsp_display_new(&cfg, &s_panel, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_display_new failed: %s", esp_err_to_name(err));
        s_panel = NULL;
        return;
    }
    esp_lcd_panel_disp_on_off(s_panel, true);
    // bsp_display_new() already inits the backlight LEDC; calling
    // bsp_display_brightness_init() again conflicts on GPIO 22.
    bsp_display_backlight_on();
    esp_err_t br = bsp_display_brightness_set(90);
    ESP_LOGI(TAG, "backlight on, brightness_set -> %s", esp_err_to_name(br));

    // Work straight in the DPI panel's own framebuffer (num_fbs = 1, PSRAM,
    // continuously scanned out) rather than esp_lcd_panel_draw_bitmap().
    void *fb = NULL;
    esp_err_t fe = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb);
    s_fb = (uint16_t *)fb;
    ESP_LOGI(TAG, "panel up (%dx%d), framebuffer=%p (%s)", PANEL_W, PANEL_H, fb, esp_err_to_name(fe));

    ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM };
    esp_err_t pe = ppa_register_client(&pc, &s_ppa);
    ESP_LOGI(TAG, "ppa_register_client -> %s", esp_err_to_name(pe));

    if (s_fb) {
        for (int i = 0; i < PANEL_W * PANEL_H; i++) {
            s_fb[i] = 0x0000;
        }
    }

    esp_err_t te = bsp_touch_new(NULL, &s_tp);
    ESP_LOGI(TAG, "bsp_touch_new -> %s", esp_err_to_name(te));
}

void DG_DrawFrame(void)
{
    if (!s_fb || !s_ppa) {
        return;
    }

    // Hardware rotate + scale + XRGB8888->RGB565 via the PPA (the software path
    // was ~400 ms/frame from strided PSRAM reads). DG_ScreenBuffer is landscape
    // RESX x RESY XRGB8888; PPA rotates 90 CCW ("left") and scales to fill the
    // native-portrait 720x1280 panel. After 90 CCW: out_w = RESY*scale_y,
    // out_h = RESX*scale_x.
    int64_t t0 = esp_timer_get_time();

    ppa_srm_oper_config_t op = {
        .in = {
            .buffer          = DG_ScreenBuffer,
            .pic_w           = DOOMGENERIC_RESX,
            .pic_h           = DOOMGENERIC_RESY,
            .block_w         = DOOMGENERIC_RESX,
            .block_h         = DOOMGENERIC_RESY,
            .srm_cm          = PPA_SRM_COLOR_MODE_ARGB8888,
        },
        .out = {
            .buffer          = s_fb,
            .buffer_size     = (size_t)PANEL_W * PANEL_H * 2,
            .pic_w           = PANEL_W,
            .pic_h           = PANEL_H,
            .srm_cm          = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = (float)PANEL_H / DOOMGENERIC_RESX,   // 1280/640 = 2.0
        .scale_y = (float)PANEL_W / DOOMGENERIC_RESY,   //  720/400 = 1.8
        .mode    = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t e = ppa_do_scale_rotate_mirror(s_ppa, &op);

    static uint32_t fr;
    if ((++fr & 63) == 0) {
        ESP_LOGI(TAG, "DG_DrawFrame ppa: %lld us (%s)",
                 (long long)(esp_timer_get_time() - t0), esp_err_to_name(e));
    }
}

void DG_SleepMs(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// GT911 gives coordinates in the panel's native portrait frame (x:0..720,
// y:0..1280). Held landscape with DOOM upright, the panel's native Y is the
// physical left<->right axis and native X is physical up<->down. Zones (as
// fractions of the physical screen); tune / mirror once tested on glass.
static void poll_touch(void)
{
    for (unsigned i = 0; i < DG_NKEYS; i++) {
        s_held[i] = false;
    }
    if (!s_tp) {
        return;
    }

    esp_lcd_touch_read_data(s_tp);
    uint16_t tx[5], ty[5];
    uint8_t cnt = 0;
    if (!esp_lcd_touch_get_coordinates(s_tp, tx, ty, NULL, &cnt, 5) || cnt == 0) {
        return;
    }

    for (int p = 0; p < cnt; p++) {
        float hx = 1.0f - (float)ty[p] / PANEL_H;   // physical left(0)..right(1)
        float hy = (float)tx[p] / PANEL_W;          // physical top(0)..bottom(1)
        int z;
        if (hy > 0.82f)        z = ZFIRE;
        else if (hx < 0.30f)   z = ZL;
        else if (hx > 0.70f)   z = ZR;
        else if (hy < 0.45f)   z = ZU;
        else                   z = ZD;
        s_held[z] = true;

        static int64_t s_last_log;
        int64_t now = esp_timer_get_time();
        if (now - s_last_log > 120000) {
            ESP_LOGI(TAG, "touch native=(%u,%u) hx=%.2f hy=%.2f zone=%d", tx[p], ty[p], hx, hy, z);
            s_last_log = now;
        }
    }
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    static int64_t s_last_poll;
    int64_t now = esp_timer_get_time();
    if (now - s_last_poll > 15000) {   // ~66 Hz touch poll
        poll_touch();
        s_last_poll = now;
    }

    for (unsigned i = 0; i < DG_NKEYS; i++) {
        if (s_held[i] != s_reported[i]) {
            s_reported[i] = s_held[i];
            *pressed = s_held[i] ? 1 : 0;
            *key = DG_KEYS[i];
            return 1;
        }
    }
    return 0;
}

void DG_SetWindowTitle(const char *title)
{
    if (title) {
        ESP_LOGI(TAG, "title: %s", title);
    }
}
