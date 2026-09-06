// DOOMAGOTCHI — DOOM node entry point (ESP32-P4 / M5Stack Tab5).
//
// Phase 0, step "PSRAM + SD + IWAD present". Brings up the Tab5 board (PSRAM
// via sdkconfig, SD via the BSP), confirms freedoom1.wad is on the card, and
// reports the heap so we can see the 6 MiB zone + 1 MiB screenbuffer will fit
// in PSRAM before we call doomgeneric_Create.
//
// Not running the game yet: DG_DrawFrame is still a stub and we haven't wired
// the IWAD path into the engine's argv. app_main still references the
// doomgeneric entry points so --gc-sections keeps the full engine linked for
// `idf.py size`.

#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"

#include "bsp/m5stack_tab5.h"

#include "doomgeneric.h"

static const char *TAG = "doomagotchi";

#define IWAD_PATH  BSP_SD_MOUNT_POINT "/freedoom1.wad"

static void report_heap(const char *when)
{
    ESP_LOGI(TAG, "[%s] heap free  internal=%u KiB  psram=%u KiB  largest-psram-block=%u KiB",
             when,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
}

void app_main(void)
{
    void *const dg_entry[] = { (void *)&doomgeneric_Create, (void *)&doomgeneric_Tick };

    ESP_LOGI(TAG, "doom-node: Phase 0 - board / PSRAM / SD / IWAD check");

    if (esp_psram_is_initialized()) {
        ESP_LOGI(TAG, "PSRAM: %u MiB initialized", (unsigned)(esp_psram_get_size() / (1024 * 1024)));
    } else {
        ESP_LOGE(TAG, "PSRAM not initialized - the 6 MiB zone alloc will fail");
    }
    report_heap("boot");

    esp_err_t err = bsp_sdcard_mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_sdcard_mount() failed: %s - is a card inserted?", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SD mounted at %s", BSP_SD_MOUNT_POINT);

        struct stat st;
        if (stat(IWAD_PATH, &st) == 0) {
            ESP_LOGI(TAG, "IWAD: %s  (%ld bytes)", IWAD_PATH, (long)st.st_size);
            FILE *f = fopen(IWAD_PATH, "rb");
            if (f) {
                char magic[5] = {0};
                size_t n = fread(magic, 1, 4, f);
                fclose(f);
                ESP_LOGI(TAG, "IWAD magic (%u): '%s' %s", (unsigned)n, magic,
                         (n == 4 && magic[1] == 'W' && magic[2] == 'A' && magic[3] == 'D')
                             ? "OK" : "-- unexpected");
            }
        } else {
            ESP_LOGW(TAG, "IWAD not found at %s", IWAD_PATH);
            ESP_LOGW(TAG, "  copy assets/freedoom1.wad to the SD card root and reboot");
        }
    }

    ESP_LOGI(TAG, "doomgeneric linked: create=%p tick=%p  (engine not invoked yet)",
             dg_entry[0], dg_entry[1]);
    ESP_LOGI(TAG, "next: hand IWAD_PATH to doomgeneric_Create, then DG_DrawFrame -> panel");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        report_heap("idle");
    }
}
