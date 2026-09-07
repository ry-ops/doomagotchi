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

static const char *TAG = "pwn";

static const char *EV_NAME[] = { "AP_SEEN", "HANDSHAKE", "PMKID", "AP_LOST", "SESSION_END" };
static const char *ENEMY_NAME[] = {
    "Zombieman", "ShotgunGuy", "Imp", "Cacodemon", "Spectre", "Baron",
};

void app_main(void)
{
    ESP_LOGI(TAG, "DOOMAGOTCHI pwn node - the airspace is the level");

    mood_init();

    bool sd = pcap_wad_start();
    ESP_LOGI(TAG, "capture-to-SD: %s", sd ? "on" : "off (no card)");

    QueueHandle_t evq = xQueueCreate(64, sizeof(struct dgm_event));
    wifi_sniff_start(evq);

    int64_t last_stats = 0, last_tick = 0;
    for (;;) {
        struct dgm_event ev;
        while (xQueueReceive(evq, &ev, pdMS_TO_TICKS(200)) == pdTRUE) {
            mood_on_event(&ev);
            const char *en = ev.event_type < 5 ? EV_NAME[ev.event_type] : "?";
            const char *cn = ev.enemy_class < 6 ? ENEMY_NAME[ev.enemy_class] : "?";
            // this is the packet we WOULD send over LoRa (Phase 3)
            ESP_LOGI(TAG, "dgm_event { v%u unit%u %-10s %-11s rssi%u hash=0x%02x }",
                     ev.version, ev.unit_id, en, cn, ev.rssi_bucket, ev.bssid_hash);
        }

        int64_t now = esp_timer_get_time();

        if (now - last_tick > 1000 * 1000) {
            last_tick = now;
            mood_tick();
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
        }
    }
}
