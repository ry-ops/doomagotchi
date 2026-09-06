// recv_wad.c — one-time bring-up helper: pull the Freedoom IWAD onto the SD
// card over the USB-Serial/JTAG console. The Tab5's USB-C is the P4's
// USB-Serial/JTAG unit (not the OTG PHY), so USB-MSC can't expose the card;
// this is the host->device path in Phase 0. Delete once the WAD is on the card.
//
// Wire protocol (host = tools/send-wad.py):
//   dev  -> "WADRECV READY\n"   (repeated ~1/s until the header arrives)
//   host -> "WADBIN <size> <crc32_hex>\n"
//   host -> <size> raw bytes
//   dev  -> "WADRESULT OK ...\n" | "WADRESULT FAIL <reason>\n"
//
// Uses the USB-Serial/JTAG *driver* (DMA-backed) for the bulk read; going
// through the VFS fread() path caps out around 30 KiB/s.

#include "recv_wad.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

static const char *TAG = "recv_wad";

#define RX_CHUNK   8192
#define RX_DRV_BUF 24576

static void say(const char *s)
{
    usb_serial_jtag_write_bytes((const uint8_t *)s, strlen(s), pdMS_TO_TICKS(300));
}

bool recv_wad_over_serial(const char *dest_path)
{
    ESP_LOGW(TAG, "IWAD missing - entering WAD receive mode");
    ESP_LOGW(TAG, "run:  python tools/send-wad.py <port> assets/freedoom1.wad");

    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);

    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = RX_DRV_BUF,
        .tx_buffer_size = 2048,
    };
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed");
        return false;
    }

    static uint8_t buf[RX_CHUNK];
    bool ok = false;
    FILE *f = NULL;
    char tmp_path[128];
    snprintf(tmp_path, sizeof(tmp_path), "%s.part", dest_path);

    // --- header ---
    char hdr[96];
    size_t hlen = 0;
    int64_t deadline = esp_timer_get_time() + 180LL * 1000 * 1000;
    int64_t next_ready = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (now > deadline) { say("WADRESULT FAIL header-timeout\n"); goto done; }
        if (hlen == 0 && now >= next_ready) { say("WADRECV READY\n"); next_ready = now + 1000 * 1000; }

        int n = usb_serial_jtag_read_bytes(buf, 1, pdMS_TO_TICKS(200));
        if (n <= 0) { continue; }
        char c = (char)buf[0];
        if (c == '\r') { continue; }
        if (c == '\n') { if (hlen == 0) { continue; } break; }
        if (hlen < sizeof(hdr) - 1) { hdr[hlen++] = c; }
    }
    hdr[hlen] = '\0';

    unsigned long size = 0, want_crc = 0;
    if (sscanf(hdr, "WADBIN %lu %lx", &size, &want_crc) != 2 || size == 0) {
        say("WADRESULT FAIL bad-header\n");
        goto done;
    }
    ESP_LOGW(TAG, "receiving %lu bytes crc=%08lx", size, want_crc);

    f = fopen(tmp_path, "wb");
    if (!f) { say("WADRESULT FAIL open-tmp\n"); goto done; }

    uint32_t crc = 0;
    unsigned long got = 0;
    int64_t last_rx = esp_timer_get_time();
    int64_t t0 = last_rx;
    while (got < size) {
        size_t want = (size - got) < RX_CHUNK ? (size_t)(size - got) : RX_CHUNK;
        int n = usb_serial_jtag_read_bytes(buf, want, pdMS_TO_TICKS(1000));
        if (n <= 0) {
            if (esp_timer_get_time() - last_rx > 20LL * 1000 * 1000) {
                fclose(f); f = NULL; remove(tmp_path);
                char m[64]; snprintf(m, sizeof(m), "WADRESULT FAIL stall-at-%lu\n", got);
                say(m);
                goto done;
            }
            continue;
        }
        if (fwrite(buf, 1, n, f) != (size_t)n) {
            fclose(f); f = NULL; remove(tmp_path);
            say("WADRESULT FAIL sd-write\n");
            goto done;
        }
        crc = esp_rom_crc32_le(crc, buf, n);
        got += n;
        last_rx = esp_timer_get_time();
        if ((got % (2u << 20)) < RX_CHUNK) {
            ESP_LOGW(TAG, "  %lu / %lu (%lld KiB/s)", got, size,
                     (long long)(got / 1024 / ((last_rx - t0) / 1000000 + 1)));
        }
    }
    fclose(f); f = NULL;

    if ((unsigned long)crc != want_crc) {
        remove(tmp_path);
        char m[80]; snprintf(m, sizeof(m), "WADRESULT FAIL crc got=%08lx want=%08lx\n",
                             (unsigned long)crc, want_crc);
        say(m);
        goto done;
    }
    remove(dest_path);
    if (rename(tmp_path, dest_path) != 0) { say("WADRESULT FAIL rename\n"); goto done; }

    {
        char m[80]; snprintf(m, sizeof(m), "WADRESULT OK %lu bytes crc=%08lx\n",
                             got, (unsigned long)crc);
        say(m);
    }
    ok = true;

done:
    if (f) { fclose(f); }
    vTaskDelay(pdMS_TO_TICKS(100));
    usb_serial_jtag_driver_uninstall();
    return ok;
}
