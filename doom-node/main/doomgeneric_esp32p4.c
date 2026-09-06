// doomgeneric_esp32p4.c — the DOOMAGOTCHI platform layer for the Tab5.
//
// This is the ONLY file we own on the DOOM side (ADR 0001). It implements the
// six DG_* functions doomgeneric factors the platform down to. Everything above
// this line is unmodified upstream.
//
// Phase 0 status: DG_GetTicksMs / DG_SleepMs are real (wired to FreeRTOS /
// esp_timer). The other four are stubs — the framebuffer blit to the MIPI-DSI
// panel and key input are the next Phase 0 steps, after we've sized the zone
// allocator from the memory map.

#include <stdint.h>

#include "doomgeneric.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "DG";

void DG_Init(void)
{
    ESP_LOGI(TAG, "DG_Init: platform stub (no display/input yet)");
}

void DG_DrawFrame(void)
{
    // Next Phase 0 step: blit DG_ScreenBuffer (DOOMGENERIC_RESX x DOOMGENERIC_RESY,
    // 32-bit RGBA) to the 1280x720 MIPI-DSI panel.
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
