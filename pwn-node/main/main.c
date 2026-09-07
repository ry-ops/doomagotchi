// DOOMAGOTCHI — pwn node entry (ESP32-S3 / M5Stack Cardputer Adv).
//
// Phase 2: passive 2.4 GHz sensor. Promiscuous capture + tier classify + EAPOL
// (wifi_sniff), capture-to-SD as CAPnnnn.WAD (pcap_wad), and the inverted-
// pwnagotchi personality (mood). The ST7789 face and the LoRa link come next;
// for now the mood + stats go to the serial log.

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "dgm_event.h"
#include "wifi_sniff.h"
#include "pcap_wad.h"
#include "mood.h"
#include "display.h"
#include "lora.h"

static const char *TAG = "pwn";

static const char *EV_NAME[] = { "AP_SEEN", "HANDSHAKE", "PMKID", "AP_LOST", "SESSION_END" };
static const char *ENEMY_NAME[] = {
    "ZOMBIEMAN", "SHOTGUN GUY", "IMP", "CACODEMON", "SPECTRE", "BARON",
};

void app_main(void)
{
    ESP_LOGI(TAG, "DOOMAGOTCHI pwn node - the airspace is the level");

    mood_init();

    bool have_lcd = display_start();
    ESP_LOGI(TAG, "status display: %s", have_lcd ? "on" : "off");

    bool sd = pcap_wad_start();
    ESP_LOGI(TAG, "capture-to-SD: %s", sd ? "on" : "off (no card)");

    bool have_lora = lora_init();   // shares the SD SPI bus; must come after pcap_wad_start
    ESP_LOGI(TAG, "LoRa TX: %s", have_lora ? "on" : "off");

    QueueHandle_t evq = xQueueCreate(64, sizeof(struct dgm_event));
    wifi_sniff_start(evq);

    const int64_t boot_us = esp_timer_get_time();
    const char *last_enemy = NULL;
    int64_t last_stats = 0, last_tick = 0;
    for (;;) {
        struct dgm_event ev;
        while (xQueueReceive(evq, &ev, pdMS_TO_TICKS(200)) == pdTRUE) {
            mood_on_event(&ev);
            const char *en = ev.event_type < 5 ? EV_NAME[ev.event_type] : "?";
            const char *cn = ev.enemy_class < 6 ? ENEMY_NAME[ev.enemy_class] : "?";
            if (ev.enemy_class < 6) last_enemy = ENEMY_NAME[ev.enemy_class];
            ESP_LOGI(TAG, "dgm_event { v%u unit%u %-10s %-11s rssi%u hash=0x%02x }",
                     ev.version, ev.unit_id, en, cn, ev.rssi_bucket, ev.bssid_hash);
            // Phase 3: actually send it. A drop is just a monster that doesn't
            // spawn (ADR 0003), so the result is advisory.
            if (have_lora) {
                lora_send_event(&ev);
            }
        }

        int64_t now = esp_timer_get_time();

        if (now - last_tick > 1000 * 1000) {
            last_tick = now;
            mood_tick();

            wifi_sniff_stats_t ss;
            wifi_sniff_get_stats(&ss);
            disp_model_t dm = {
                .channel = ss.channel,
                .aps = ss.aps,
                .handshakes = ss.handshakes,
                .pmkids = ss.pmkids,
                .uptime_s = (uint32_t)((now - boot_us) / 1000000),
                .last_enemy = last_enemy,
            };
            mood_get(&dm.mood);
            display_render(&dm);
        }

        if (now - last_stats > 5LL * 1000 * 1000) {
            last_stats = now;
            wifi_sniff_stats_t s;
            wifi_sniff_get_stats(&s);
            pcap_wad_stats_t w;
            pcap_wad_get_stats(&w);
            mood_state_t m;
            mood_get(&m);
            ESP_LOGI(TAG, "ch%2u  frames=%lu beacons=%lu eapol=%lu aps=%lu  hs=%lu pmkid=%lu  rssi=%d",
                     s.channel, (unsigned long)s.frames, (unsigned long)s.beacons,
                     (unsigned long)s.eapol, (unsigned long)s.aps,
                     (unsigned long)s.handshakes, (unsigned long)s.pmkids, s.last_rssi);
            if (w.mounted) {
                ESP_LOGI(TAG, "  CAP%04u.WAD  %lu frames  %lu KiB  (dropped %lu)",
                         w.file_index, (unsigned long)w.written,
                         (unsigned long)(w.bytes >> 10), (unsigned long)w.dropped);
            }
            ESP_LOGI(TAG, "  [%-8s i%3u]  %lu APs/1m  %lu caps/2m  \"%s\"",
                     m.label, m.intensity, (unsigned long)m.aps_1m,
                     (unsigned long)m.caps_2m, m.quip);
            if (have_lora) {
                lora_stats_t l;
                lora_get_stats(&l);
                ESP_LOGI(TAG, "  LoRa TX  sent=%lu  timeouts=%lu  errs=%lu  last=%lums",
                         (unsigned long)l.sent, (unsigned long)l.tx_timeouts,
                         (unsigned long)l.errors, (unsigned long)l.last_airtime_ms);
            }
        }
    }
}
