// kbd_m5kbd.c — official M5Stack Tab5 Keyboard (SKU A164).
//
// STM32F030 keypad controller, I2C addr 0x6D on the Tab5 keyboard bus
// (SDA=GPIO0, SCL=GPIO1, INT=GPIO50 - see docs.m5stack.com/en/tab5/Tab5_Keyboard
// and m5stack/M5Tab5-Keyboard-Internal-FW user_i2c_reg.h). We put it in HID
// mode so the events are standard USB HID [modifier][usage] pairs and reuse
// the USB keyboard's usage->DOOM mapping. Cheat codes are handled by the
// engine (st_stuff ST_Responder) once ASCII keydowns flow.
//
// Register map (M5 FW): 0x02 = pending event count; 0x10 = keyboard mode
// (0 KEY / 1 HID / 2 CHAR); 0x30 = read [modifier, usage], 0xFF/0xFF if empty.

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

#include "kbd_usb.h"     // kbd_hid_usage_to_doom()
#include "doomkeys.h"
#include "kbd_m5kbd.h"

static const char *TAG = "m5kbd";

#define KB_SDA_GPIO   0
#define KB_SCL_GPIO   1
#define KB_I2C_PORT   0        // esp-bsp SYS bus owns port 1
#define KB_ADDR       0x6D

#define REG_EVENT_NUM     0x02
#define REG_KEYBOARD_MODE 0x10
#define REG_HID_EVENT     0x30
#define MODE_HID          0x01

static i2c_master_dev_handle_t s_dev;

#define KBQ 64
static volatile struct { uint8_t pressed; unsigned char key; } s_q[KBQ];
static volatile uint32_t s_head, s_tail;

static void q_push(uint8_t pressed, unsigned char key)
{
    uint32_t h = s_head, n = (h + 1) & (KBQ - 1);
    if (n == s_tail) { return; }
    s_q[h].pressed = pressed;
    s_q[h].key = key;
    s_head = n;
}

bool kbd_m5kbd_poll(int *pressed, unsigned char *key)
{
    uint32_t t = s_tail;
    if (t == s_head) { return false; }
    *pressed = s_q[t].pressed;
    *key = s_q[t].key;
    s_tail = (t + 1) & (KBQ - 1);
    return true;
}

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100);
}
static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 100);
}

// HID modifier byte -> emit KEY_FIRE / KEY_RSHIFT / KEY_RALT edges.
static void diff_mods(uint8_t prev, uint8_t cur)
{
    struct { uint8_t mask; unsigned char key; } m[] = {
        { 0x01 | 0x10, KEY_FIRE },    // L/R ctrl
        { 0x02 | 0x20, KEY_RSHIFT },  // L/R shift
        { 0x04 | 0x40, KEY_RALT },    // L/R alt
    };
    for (int i = 0; i < 3; i++) {
        bool was = (prev & m[i].mask) != 0;
        bool now = (cur & m[i].mask) != 0;
        if (now != was) { q_push(now ? 1 : 0, m[i].key); }
    }
}

static void kb_task(void *arg)
{
    (void)arg;
    // HID mode; this also resets the controller's event FIFO.
    if (wr(REG_KEYBOARD_MODE, MODE_HID) != ESP_OK) {
        ESP_LOGW(TAG, "set HID mode failed - keyboard attached @0x%02x?", KB_ADDR);
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t prev_mod = 0, prev_kc = 0;

    for (;;) {
        uint8_t cnt = 0;
        if (rd(REG_EVENT_NUM, &cnt, 1) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(200));  // no keyboard - back off
            continue;
        }
        while (cnt-- > 0) {
            uint8_t ev[2];
            if (rd(REG_HID_EVENT, ev, 2) != ESP_OK) { break; }
            if (ev[0] == 0xFF && ev[1] == 0xFF) { break; }  // empty

            uint8_t mod = ev[0], kc = ev[1];
            ESP_LOGI(TAG, "hid ev: mod=0x%02x usage=0x%02x -> doom=0x%02x",
                     mod, kc, kbd_hid_usage_to_doom(kc));
            diff_mods(prev_mod, mod);
            prev_mod = mod;

            if (kc != prev_kc) {
                if (prev_kc) {
                    unsigned char k = kbd_hid_usage_to_doom(prev_kc);
                    if (k) { q_push(0, k); }
                }
                if (kc) {
                    unsigned char k = kbd_hid_usage_to_doom(kc);
                    if (k) { q_push(1, k); }
                }
                prev_kc = kc;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(12));
    }
}

void kbd_m5kbd_start(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = KB_I2C_PORT,
        .sda_io_num = KB_SDA_GPIO,
        .scl_io_num = KB_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    esp_err_t e = i2c_new_master_bus(&bus_cfg, &bus);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus (sda%d/scl%d port%d) -> %s",
                 KB_SDA_GPIO, KB_SCL_GPIO, KB_I2C_PORT, esp_err_to_name(e));
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = KB_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "add_device 0x%02x failed", KB_ADDR);
        return;
    }

    esp_err_t probe = i2c_master_probe(bus, KB_ADDR, 100);
    ESP_LOGI(TAG, "M5 keyboard @0x%02x (sda%d/scl%d): %s",
             KB_ADDR, KB_SDA_GPIO, KB_SCL_GPIO,
             probe == ESP_OK ? "present" : "no ACK (plug it in / check bus)");

    xTaskCreatePinnedToCore(kb_task, "m5kbd", 3072, NULL, 4, NULL, 0);
}
