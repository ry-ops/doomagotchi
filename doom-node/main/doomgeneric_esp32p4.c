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

#include "bsp/display.h"
#include "bsp/m5stack_tab5.h"

static const char *TAG = "DG";

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_rgb565;   // DOOMGENERIC_RESX*RESY, PSRAM

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

    s_rgb565 = heap_caps_malloc((size_t)DOOMGENERIC_RESX * DOOMGENERIC_RESY * 2,
                                MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "panel up (%dx%d), rgb565 scratch %p", BSP_LCD_H_RES, BSP_LCD_V_RES, s_rgb565);
}

void DG_DrawFrame(void)
{
    if (!s_panel || !s_rgb565) {
        return;
    }

    // DG_ScreenBuffer is XRGB8888 (i_video.c: red_off 16, green_off 8, blue_off 0).
    const uint32_t *src = (const uint32_t *)DG_ScreenBuffer;
    const int n = DOOMGENERIC_RESX * DOOMGENERIC_RESY;
    for (int i = 0; i < n; i++) {
        uint32_t p = src[i];
        s_rgb565[i] = (uint16_t)(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
    }

    // First pass: straight 1:1 into the top-left of the (portrait) panel.
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, DOOMGENERIC_RESX, DOOMGENERIC_RESY, s_rgb565);
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
