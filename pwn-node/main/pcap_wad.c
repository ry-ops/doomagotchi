#include "pcap_wad.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

static const char *TAG = "pcap";

// ---- Cardputer Adv microSD (SPI, its own bus) -----------------------------
#define SD_SCK   40
#define SD_MISO  39
#define SD_MOSI  14
#define SD_CS    12
#define SD_HOST  SPI2_HOST

#define MOUNT_PT   "/sdcard"
#define SNAPLEN    2304                 // 802.11 max MSDU + headers, plenty
#define ROTATE_BYTES (4u * 1024 * 1024) // new CAPnnnn.WAD every 4 MiB

// ---- frame pool + queue -------------------------------------------------
#define SLOT_LEN  512
#define SLOTS     24
typedef struct {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint16_t len;                       // original frame length
    uint16_t incl;                      // bytes actually copied (<= SLOT_LEN)
    uint8_t  data[SLOT_LEN];
} slot_t;

static slot_t        s_pool[SLOTS];
static QueueHandle_t s_freeq;           // indices ready to fill
static QueueHandle_t s_workq;           // indices ready to write
static pcap_wad_stats_t s_stats;
static bool s_ready;

// pcap (classic) little-endian global header, linktype 105 = LINKTYPE_IEEE802_11
static const uint8_t PCAP_HDR[24] = {
    0xd4, 0xc3, 0xb2, 0xa1,             // magic (usec)
    0x02, 0x00, 0x04, 0x00,             // version 2.4
    0x00, 0x00, 0x00, 0x00,             // thiszone
    0x00, 0x00, 0x00, 0x00,             // sigfigs
    (SNAPLEN & 0xff), (SNAPLEN >> 8), 0x00, 0x00,
    105, 0x00, 0x00, 0x00,
};

static FILE *s_fp;

// The SD card and the LoRa CAP (SX1262) share one SPI bus (SCK 40 / MOSI 14 /
// MISO 39). The SX1262's CS is GPIO5; left floating it idles low, holds the
// radio selected, and corrupts every SD transaction (send_if_cond times out,
// 0x108). Park CS high before touching the bus. GPIO3 is the SX1262 RST -
// also park it high (inactive). Everything else on the CAP header (GPIO4 DIO1,
// GPIO6 BUSY, GPIO13 GPS-TX) is an *output from the CAP* - do NOT drive it.
static void board_park_lora_cap(void)
{
    static const int park_high[] = { 5 /* SX1262 NSS */, 3 /* SX1262 RST */ };
    uint64_t mask = 0;
    for (size_t i = 0; i < sizeof(park_high) / sizeof(park_high[0]); i++)
        mask |= 1ULL << park_high[i];
    gpio_config_t c = { .pin_bit_mask = mask, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&c);
    for (size_t i = 0; i < sizeof(park_high) / sizeof(park_high[0]); i++)
        gpio_set_level(park_high[i], 1);
}

static bool open_next_file(void)
{
    if (s_fp) { fclose(s_fp); s_fp = NULL; }
    char path[32];
    // find the first free index so we don't clobber prior sessions
    for (int tries = 0; tries < 10000; tries++) {
        snprintf(path, sizeof(path), MOUNT_PT "/CAP%04u.WAD", s_stats.file_index);
        FILE *probe = fopen(path, "rb");
        if (!probe) break;
        fclose(probe);
        s_stats.file_index++;
    }
    s_fp = fopen(path, "wb");
    if (!s_fp) { ESP_LOGE(TAG, "open %s failed", path); return false; }
    fwrite(PCAP_HDR, 1, sizeof(PCAP_HDR), s_fp);
    fflush(s_fp);
    s_stats.bytes = sizeof(PCAP_HDR);
    ESP_LOGI(TAG, "capturing to %s", path);
    return true;
}

static void writer_task(void *arg)
{
    if (!open_next_file()) { s_ready = false; vTaskDelete(NULL); return; }

    uint32_t since_flush = 0;
    for (;;) {
        int idx;
        if (xQueueReceive(s_workq, &idx, portMAX_DELAY) != pdTRUE) continue;
        slot_t *sl = &s_pool[idx];

        uint8_t rec[16];
        rec[0]  = sl->ts_sec;  rec[1]  = sl->ts_sec >> 8;
        rec[2]  = sl->ts_sec >> 16; rec[3] = sl->ts_sec >> 24;
        rec[4]  = sl->ts_usec; rec[5] = sl->ts_usec >> 8;
        rec[6]  = sl->ts_usec >> 16; rec[7] = sl->ts_usec >> 24;
        rec[8]  = sl->incl;    rec[9]  = sl->incl >> 8; rec[10] = 0; rec[11] = 0;
        rec[12] = sl->len;     rec[13] = sl->len >> 8;  rec[14] = 0; rec[15] = 0;

        fwrite(rec, 1, sizeof(rec), s_fp);
        fwrite(sl->data, 1, sl->incl, s_fp);
        s_stats.bytes += sizeof(rec) + sl->incl;
        s_stats.written++;

        int free_idx = idx;
        xQueueSend(s_freeq, &free_idx, 0);

        if (++since_flush >= 16) { fflush(s_fp); since_flush = 0; }
        if (s_stats.bytes >= ROTATE_BYTES) {
            s_stats.file_index++;
            open_next_file();
        }
    }
}

bool pcap_wad_start(void)
{
    if (s_ready) return true;

    board_park_lora_cap();

    // Clear any prior config on the CS pin, then let the sdspi driver own it
    // (touching it ourselves after this races the driver -> "GPIO conflict").
    gpio_reset_pin(SD_CS);

    spi_bus_config_t bus = {
        .mosi_io_num = SD_MOSI,
        .miso_io_num = SD_MISO,
        .sclk_io_num = SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 8192,
    };
    esp_err_t e = spi_bus_initialize(SD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "spi bus init: %s", esp_err_to_name(e));
        return false;
    }

    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = SD_CS;
    dev.host_id = SD_HOST;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST;
    host.max_freq_khz = 20000;          // conservative; card + wiring vary

    const esp_vfs_fat_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card;
    e = esp_vfs_fat_sdspi_mount(MOUNT_PT, &host, &dev, &mcfg, &card);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed (%s) - capture disabled, sensor runs on",
                 esp_err_to_name(e));
        return false;
    }
    ESP_LOGI(TAG, "SD mounted: %s %lluMB", card->cid.name,
             ((uint64_t)card->csd.capacity * card->csd.sector_size) >> 20);

    s_freeq = xQueueCreate(SLOTS, sizeof(int));
    s_workq = xQueueCreate(SLOTS, sizeof(int));
    for (int i = 0; i < SLOTS; i++) xQueueSend(s_freeq, &i, 0);

    s_stats.mounted = true;
    s_ready = true;
    xTaskCreatePinnedToCore(writer_task, "pcap_wr", 4096, NULL, 4, NULL, 1);
    return true;
}

void pcap_wad_offer(const uint8_t *frame, uint16_t len, int64_t ts_us)
{
    if (!s_ready) return;

    int idx;
    if (xQueueReceive(s_freeq, &idx, 0) != pdTRUE) { s_stats.dropped++; return; }

    slot_t *sl = &s_pool[idx];
    uint16_t incl = len > SLOT_LEN ? SLOT_LEN : len;
    sl->ts_sec  = (uint32_t)(ts_us / 1000000);
    sl->ts_usec = (uint32_t)(ts_us % 1000000);
    sl->len  = len;
    sl->incl = incl;
    memcpy(sl->data, frame, incl);

    if (xQueueSend(s_workq, &idx, 0) != pdTRUE) {
        xQueueSend(s_freeq, &idx, 0);   // give the slot back
        s_stats.dropped++;
    }
}

void pcap_wad_get_stats(pcap_wad_stats_t *out) { *out = s_stats; }
