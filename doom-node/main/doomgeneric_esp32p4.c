// doomgeneric_esp32p4.c — the DOOMAGOTCHI platform layer for the Tab5.
//
// The only file we own on the DOOM side (ADR 0001): the six DG_* functions.
// Everything above this line is unmodified upstream doomgeneric.
//
// Phase 0 status: timers real; DG_DrawFrame blits DG_ScreenBuffer to the
// MIPI-DSI panel (first pass: 1:1, top-left, portrait — rotation/scale next);
// key input still stubbed.

#include <stdint.h>
#include <string.h>

#include "doomgeneric.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "driver/ppa.h"

#include "bsp/display.h"
#include "bsp/m5stack_tab5.h"

static const char *TAG = "DG";

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;       // the DPI panel's real framebuffer, 720x1280 RGB565
static ppa_client_handle_t s_ppa;

#define PANEL_W  720   // BSP_LCD_H_RES  (native portrait)
#define PANEL_H  1280  // BSP_LCD_V_RES

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

int DG_GetKey(int *pressed, unsigned char *key)
{
    (void)pressed;
    (void)key;
    return 0; // no key events yet
}

void DG_SetWindowTitle(const char *title)
{
    if (title) {
        ESP_LOGI(TAG, "title: %s", title);
    }
}
