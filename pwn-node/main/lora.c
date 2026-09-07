#include "lora.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"

static const char *TAG = "lora";

// ---- CAP header pins (shared SD bus + SX1262 control) --------------------
#define LORA_SPI_HOST   SPI2_HOST   // MUST match pcap_wad.c SD_HOST
#define PIN_SCK   40
#define PIN_MOSI  14
#define PIN_MISO  39
#define PIN_NSS    5
#define PIN_RST    3
#define PIN_BUSY   6
// PIN_DIO1 (4) is a CAP output (TxDone IRQ). We poll GetIrqStatus instead of
// wiring it, so it's left unconfigured - see the note in the last SD-rail fix.

// ---- on-air config (see lora.h header block) ----------------------------
#define LORA_FREQ_HZ     915000000UL
#define LORA_TX_DBM      14
#define LORA_USE_TCXO    1           // M5 LoRa-1262 carries a TCXO on DIO3
#define LORA_TCXO_V      0x02        // 1.8 V
#define LORA_SYNCWORD    0x1424      // private network
#define LORA_PREAMBLE    12
#define LORA_PAYLOAD_LEN ((uint8_t)sizeof(struct dgm_event))  // 6

// ---- SX126x opcodes ----------------------------------------------------
#define OP_SET_SLEEP              0x84
#define OP_SET_STANDBY           0x80
#define OP_SET_TX                0x83
#define OP_SET_REGULATOR_MODE    0x96
#define OP_CALIBRATE             0x89
#define OP_CALIBRATE_IMAGE       0x98
#define OP_SET_PA_CONFIG         0x95
#define OP_SET_DIO_IRQ_PARAMS    0x08
#define OP_GET_IRQ_STATUS        0x12
#define OP_CLR_IRQ_STATUS        0x02
#define OP_SET_DIO2_RFSW         0x9D
#define OP_SET_DIO3_TCXO         0x97
#define OP_SET_RF_FREQUENCY      0x86
#define OP_SET_PACKET_TYPE       0x8A
#define OP_SET_TX_PARAMS         0x8E
#define OP_SET_MODULATION_PARAMS 0x8B
#define OP_SET_PACKET_PARAMS     0x8C
#define OP_SET_BUFFER_BASE       0x8F
#define OP_WRITE_BUFFER          0x0E
#define OP_WRITE_REGISTER        0x0D
#define OP_READ_REGISTER         0x1D
#define OP_GET_STATUS            0xC0
#define OP_GET_DEVICE_ERRORS     0x17
#define OP_CLR_DEVICE_ERRORS     0x07

#define STDBY_RC   0x00
#define PACKET_TYPE_LORA 0x01

#define REG_LORA_SYNCWORD_MSB 0x0740
#define REG_LORA_SYNCWORD_LSB 0x0741

#define IRQ_TX_DONE 0x0001
#define IRQ_TIMEOUT 0x0200

// LoRa modulation param encodings
#define LORA_SF9    0x09
#define LORA_BW_125 0x04
#define LORA_CR_4_5 0x01
#define RAMP_200U   0x04

static spi_device_handle_t s_spi;
static lora_stats_t        s_stats;

// -------------------------------------------------------------------------

static void wait_busy(const char *what)
{
    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(PIN_BUSY)) {
        if (esp_timer_get_time() - t0 > 20000) {   // 20 ms
            ESP_LOGW(TAG, "BUSY stuck high (%s)", what ? what : "?");
            return;
        }
        esp_rom_delay_us(40);
    }
}

static esp_err_t spi_xfer(const uint8_t *tx, uint8_t *rx, size_t n)
{
    spi_transaction_t t = {
        .length    = n * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

// Command with parameters, no response payload.
static void sx_cmd(uint8_t op, const uint8_t *args, size_t n)
{
    uint8_t buf[1 + 16];
    buf[0] = op;
    if (n) {
        memcpy(&buf[1], args, n);
    }
    wait_busy("cmd");
    esp_err_t e = spi_xfer(buf, NULL, 1 + n);
    if (e != ESP_OK) {
        s_stats.errors++;
    }
}

// "Get" command: [op][status][payload..n]. Copies the n payload bytes.
static void sx_get(uint8_t op, uint8_t *out, size_t n)
{
    uint8_t tx[2 + 8] = { 0 };
    uint8_t rx[2 + 8] = { 0 };
    tx[0] = op;
    wait_busy("get");
    if (spi_xfer(tx, rx, 2 + n) != ESP_OK) {
        s_stats.errors++;
        memset(out, 0, n);
        return;
    }
    memcpy(out, &rx[2], n);
}

static void sx_write_reg(uint16_t addr, const uint8_t *data, size_t n)
{
    uint8_t buf[3 + 8];
    buf[0] = OP_WRITE_REGISTER;
    buf[1] = addr >> 8;
    buf[2] = addr & 0xff;
    memcpy(&buf[3], data, n);
    wait_busy("wreg");
    if (spi_xfer(buf, NULL, 3 + n) != ESP_OK) {
        s_stats.errors++;
    }
}

static void sx_read_reg(uint16_t addr, uint8_t *out, size_t n)
{
    uint8_t tx[4 + 8] = { 0 };
    uint8_t rx[4 + 8] = { 0 };
    tx[0] = OP_READ_REGISTER;
    tx[1] = addr >> 8;
    tx[2] = addr & 0xff;
    wait_busy("rreg");
    if (spi_xfer(tx, rx, 4 + n) != ESP_OK) {
        s_stats.errors++;
        memset(out, 0, n);
        return;
    }
    memcpy(out, &rx[4], n);   // [op][addr_hi][addr_lo][status][data..]
}

static void sx_write_buffer(uint8_t offset, const uint8_t *data, size_t n)
{
    uint8_t buf[2 + 16];
    buf[0] = OP_WRITE_BUFFER;
    buf[1] = offset;
    memcpy(&buf[2], data, n);
    wait_busy("wbuf");
    if (spi_xfer(buf, NULL, 2 + n) != ESP_OK) {
        s_stats.errors++;
    }
}

static uint16_t sx_irq_status(void)
{
    uint8_t b[2];
    sx_get(OP_GET_IRQ_STATUS, b, 2);
    return ((uint16_t)b[0] << 8) | b[1];
}

static void sx_clear_irq(uint16_t mask)
{
    uint8_t a[2] = { mask >> 8, mask & 0xff };
    sx_cmd(OP_CLR_IRQ_STATUS, a, 2);
}

static uint16_t sx_device_errors(void)
{
    uint8_t b[2];
    sx_get(OP_GET_DEVICE_ERRORS, b, 2);
    return ((uint16_t)b[0] << 8) | b[1];
}

static void sx_standby(void)
{
    uint8_t m = STDBY_RC;
    sx_cmd(OP_SET_STANDBY, &m, 1);
}

// -------------------------------------------------------------------------

static void hw_reset(void)
{
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    wait_busy("reset");
}

static void radio_config(void)
{
    sx_standby();

    uint8_t reg = 0x01;   // DC-DC + LDO
    sx_cmd(OP_SET_REGULATOR_MODE, &reg, 1);

#if LORA_USE_TCXO
    // DIO3 supplies the TCXO. voltage=1.8V, startup delay ~10 ms in 15.625us units.
    uint8_t tcxo[4] = { LORA_TCXO_V, 0x00, 0x02, 0x80 };
    sx_cmd(OP_SET_DIO3_TCXO, tcxo, 4);
    uint8_t clr[2] = { 0x00, 0x00 };
    sx_cmd(OP_CLR_DEVICE_ERRORS, clr, 2);
    uint8_t cal = 0x7f;   // recalibrate all blocks after switching to TCXO
    sx_cmd(OP_CALIBRATE, &cal, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    wait_busy("cal");
#endif

    uint8_t rfsw = 0x01;  // DIO2 drives the RF switch
    sx_cmd(OP_SET_DIO2_RFSW, &rfsw, 1);

    uint8_t pt = PACKET_TYPE_LORA;
    sx_cmd(OP_SET_PACKET_TYPE, &pt, 1);

    uint8_t img[2] = { 0xE1, 0xE9 };   // 902-928 MHz image calibration
    sx_cmd(OP_CALIBRATE_IMAGE, img, 2);

    uint32_t frf = (uint32_t)(((uint64_t)LORA_FREQ_HZ << 25) / 32000000UL);
    uint8_t f[4] = { frf >> 24, frf >> 16, frf >> 8, frf };
    sx_cmd(OP_SET_RF_FREQUENCY, f, 4);

    // SX1262 PA. +14 dBm: paDutyCycle 0x02, hpMax 0x02, deviceSel 0 (SX1262).
    uint8_t pa[4] = { 0x02, 0x02, 0x00, 0x01 };
    sx_cmd(OP_SET_PA_CONFIG, pa, 4);
    uint8_t txp[2] = { (uint8_t)(int8_t)LORA_TX_DBM, RAMP_200U };
    sx_cmd(OP_SET_TX_PARAMS, txp, 2);

    uint8_t base[2] = { 0x00, 0x00 };
    sx_cmd(OP_SET_BUFFER_BASE, base, 2);

    uint8_t mod[4] = { LORA_SF9, LORA_BW_125, LORA_CR_4_5, 0x00 /* no LDRO */ };
    sx_cmd(OP_SET_MODULATION_PARAMS, mod, 4);

    uint8_t pkt[6] = {
        (LORA_PREAMBLE >> 8) & 0xff, LORA_PREAMBLE & 0xff,
        0x00,                 // explicit header
        LORA_PAYLOAD_LEN,
        0x01,                 // payload CRC on
        0x00,                 // standard IQ
    };
    sx_cmd(OP_SET_PACKET_PARAMS, pkt, 6);

    uint8_t sw[2] = { (LORA_SYNCWORD >> 8) & 0xff, LORA_SYNCWORD & 0xff };
    sx_write_reg(REG_LORA_SYNCWORD_MSB, &sw[0], 1);
    sx_write_reg(REG_LORA_SYNCWORD_LSB, &sw[1], 1);

    uint8_t irq[8] = {
        (IRQ_TX_DONE | IRQ_TIMEOUT) >> 8, (IRQ_TX_DONE | IRQ_TIMEOUT) & 0xff,
        (IRQ_TX_DONE | IRQ_TIMEOUT) >> 8, (IRQ_TX_DONE | IRQ_TIMEOUT) & 0xff,
        0x00, 0x00, 0x00, 0x00,
    };
    sx_cmd(OP_SET_DIO_IRQ_PARAMS, irq, 8);
}

// One transmit. Returns false on SPI error or no TxDone before the deadline.
static bool tx_payload(const uint8_t *payload, size_t n)
{
    wait_busy("tx-pre");
    sx_standby();
    sx_clear_irq(0xffff);
    sx_write_buffer(0x00, payload, n);

    // Re-assert length in case a future caller varies it.
    uint8_t pkt[6] = {
        (LORA_PREAMBLE >> 8) & 0xff, LORA_PREAMBLE & 0xff,
        0x00, (uint8_t)n, 0x01, 0x00,
    };
    sx_cmd(OP_SET_PACKET_PARAMS, pkt, 6);

    // SetTx with a 1 s chip-side timeout (units of 15.625 us) so the PA can
    // never stay keyed if our poll wedges. 1 s = 0x00FA00.
    uint8_t tset[3] = { 0x00, 0xFA, 0x00 };
    int64_t t0 = esp_timer_get_time();
    sx_cmd(OP_SET_TX, tset, 3);

    for (;;) {
        uint16_t irq = sx_irq_status();
        if (irq & IRQ_TX_DONE) {
            s_stats.last_airtime_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
            s_stats.sent++;
            sx_clear_irq(0xffff);
            sx_standby();
            return true;
        }
        if (irq & IRQ_TIMEOUT) {
            s_stats.tx_timeouts++;
            sx_clear_irq(0xffff);
            sx_standby();
            return false;
        }
        if (esp_timer_get_time() - t0 > 1500000) {   // 1.5 s wall clock
            s_stats.tx_timeouts++;
            sx_clear_irq(0xffff);
            sx_standby();
            return false;
        }
        esp_rom_delay_us(500);
    }
}

// -------------------------------------------------------------------------

bool lora_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(PIN_RST, 1);

    io.pin_bit_mask = (1ULL << PIN_BUSY);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io);

    // The bus is normally already up (pcap_wad_start ran first); tolerate that.
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 8192,
    };
    esp_err_t e = spi_bus_initialize(LORA_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "spi bus init: %s - LoRa disabled, sensor runs on", esp_err_to_name(e));
        return false;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 8 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_NSS,
        .queue_size = 1,
    };
    if (spi_bus_add_device(LORA_SPI_HOST, &dev, &s_spi) != ESP_OK) {
        ESP_LOGW(TAG, "spi_bus_add_device failed - LoRa disabled, sensor runs on");
        return false;
    }

    hw_reset();

    // Does anything answer? Write the sync word and read it back.
    radio_config();
    uint8_t rb[1] = { 0 };
    sx_read_reg(REG_LORA_SYNCWORD_MSB, rb, 1);
    uint16_t derr = sx_device_errors();

    if (rb[0] != ((LORA_SYNCWORD >> 8) & 0xff)) {
        ESP_LOGW(TAG, "SX1262 not responding (syncword rb=0x%02x, want 0x%02x) - LoRa disabled",
                 rb[0], (LORA_SYNCWORD >> 8) & 0xff);
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
        return false;
    }
    if (derr) {
        // Non-fatal, but tells us if the TCXO assumption is wrong (XOSC_START_ERR
        // is bit 5 = 0x0020). Log it so the first flash is diagnostic.
        ESP_LOGW(TAG, "SX1262 device errors = 0x%04x (bit5 set => TCXO/XOSC issue,"
                      " try LORA_USE_TCXO=0)", derr);
    }

    // Antenna is attached: prove the PA keys and TxDone fires. Send a benign
    // probe - an AP_LOST for a zero BSSID that the RX side will simply not match.
    struct dgm_event probe = {
        .version = DGM_PROTO_VERSION,
        .unit_id = 0,
        .event_type = DGM_EV_AP_LOST,
        .enemy_class = DGM_ENEMY_ZOMBIEMAN,
        .rssi_bucket = 0,
        .bssid_hash = 0,
    };
    bool tx_ok = tx_payload((const uint8_t *)&probe, sizeof(probe));

    s_stats.present = true;
    ESP_LOGI(TAG, "SX1262 up: 915.000 MHz SF9 BW125 CR4/5 +%d dBm, probe TX %s (%lu ms)",
             LORA_TX_DBM, tx_ok ? "OK" : "NO TxDone",
             (unsigned long)s_stats.last_airtime_ms);
    return true;
}

bool lora_send_event(const struct dgm_event *ev)
{
    if (!s_spi || !s_stats.present) {
        return false;
    }
    return tx_payload((const uint8_t *)ev, sizeof(*ev));
}

void lora_get_stats(lora_stats_t *out)
{
    *out = s_stats;
}
