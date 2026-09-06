// DOOMAGOTCHI — DOOM node entry point (ESP32-P4 / M5Stack Tab5).
//
// Phase 0, step "DOOM parses the WAD + inits the zone". Brings up the board,
// makes sure /sdcard/freedoom1.wad exists (receiving it over the console the
// first time), then hands it to doomgeneric and runs the tic loop.
//
// Still headless: DG_DrawFrame is a stub, DG_GetKey returns nothing. Success
// here = the startup banner, W_Init / Z_Init / R_Init / P_Init all pass, and
// E1M1 sets up and renders (into DG_ScreenBuffer) without an I_Error/panic.

#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_system.h"

#include "bsp/m5stack_tab5.h"

#include "doomgeneric.h"
#include "recv_wad.h"

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

// The engine runs in its own task: D_DoomMain -> ... -> D_DoomLoop does its
// init and one doomgeneric_Tick(), then returns; we drive frames after that.
// Big stack: R_RenderBSPNode recurses over the level's BSP tree.
static void doom_task(void *arg)
{
    (void)arg;
    static char *argv[] = {
        "doomgeneric",
        "-iwad", IWAD_PATH,
        "-warp", "1", "1",   // straight into E1M1 (skip title/demo for this test)
    };

    ESP_LOGI(TAG, "doomgeneric_Create(%d, ...) argv: -iwad %s -warp 1 1",
             (int)(sizeof(argv) / sizeof(argv[0])), IWAD_PATH);

    doomgeneric_Create(sizeof(argv) / sizeof(argv[0]), argv);

    ESP_LOGI(TAG, "doomgeneric_Create returned - entering tick loop");
    report_heap("post-Create");

    uint32_t frame = 0;
    for (;;) {
        doomgeneric_Tick();
        if ((++frame % 35) == 0) {
            ESP_LOGI(TAG, "tick %lu  (internal free %u KiB)", (unsigned long)frame,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        }
        vTaskDelay(1);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "doom-node: Phase 0 - run DOOM headless");

    if (esp_psram_is_initialized()) {
        ESP_LOGI(TAG, "PSRAM: %u MiB initialized", (unsigned)(esp_psram_get_size() / (1024 * 1024)));
    } else {
        ESP_LOGE(TAG, "PSRAM not initialized - the 6 MiB zone alloc will fail");
    }
    report_heap("boot");

    esp_err_t err = bsp_sdcard_mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_sdcard_mount() failed: %s - is a card inserted?", esp_err_to_name(err));
        goto idle;
    }
    ESP_LOGI(TAG, "SD mounted at %s", BSP_SD_MOUNT_POINT);

    struct stat st;
    if (stat(IWAD_PATH, &st) != 0) {
        ESP_LOGW(TAG, "IWAD not found at %s", IWAD_PATH);
        if (recv_wad_over_serial(IWAD_PATH)) {
            ESP_LOGI(TAG, "WAD received - rebooting to load it");
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
        ESP_LOGW(TAG, "no WAD - staying idle; re-run tools/send-wad.py and reboot");
        goto idle;
    }
    ESP_LOGI(TAG, "IWAD: %s  (%ld bytes)", IWAD_PATH, (long)st.st_size);

    xTaskCreatePinnedToCore(doom_task, "doom", 64 * 1024, NULL, 5, NULL, 1);

idle:
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        report_heap("idle");
    }
}
