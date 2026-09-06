// DOOMAGOTCHI — DOOM node entry point (ESP32-P4 / M5Stack Tab5).
//
// Phase 0, step "builds and links against doomgeneric". We do NOT run the game
// yet: doomgeneric_Create() -> D_DoomMain() needs the Freedoom IWAD (next step)
// and DG_DrawFrame is still a stub. What this file does is reference the
// doomgeneric entry points so the linker's --gc-sections keeps the full engine
// object graph in the image — everything is statically reachable from
// doomgeneric_Create() -> D_DoomMain(), so `idf.py size` then reports the real
// DOOM footprint we size the zone allocator against.

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "doomgeneric.h"

static const char *TAG = "doomagotchi";

void app_main(void)
{
    // Defeat --gc-sections without invoking: taking the address of these keeps
    // doomgeneric_Create/_Tick (and thus the whole reachable engine) linked in.
    void *const dg_entry[] = { (void *)&doomgeneric_Create, (void *)&doomgeneric_Tick };

    ESP_LOGI(TAG, "doom-node: Phase 0 link check");
    ESP_LOGI(TAG, "doomgeneric linked: create=%p tick=%p  DG_* stubs active",
             dg_entry[0], dg_entry[1]);
    ESP_LOGI(TAG, "next: SD/IWAD file shim + DG_DrawFrame to the MIPI-DSI panel");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
