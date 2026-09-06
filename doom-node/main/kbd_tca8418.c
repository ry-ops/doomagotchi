// kbd_tca8418.c — the official M5Stack Tab5 pin-connector keyboard.
//
// It's a TCA8418 I2C keypad scanner (addr 0x34) on the Tab5's "EXT" I2C bus
// (SDA=GPIO53, SCL=GPIO54 per M5's bsp/m5stack_tab5.h; the espressif esp-bsp
// only wires its own SYS bus on GPIO31/32 port 1). 8x9 matrix. Register map +
// sequence + physical keycode->char table ported from M5's M5Tab5-UserDemo
// (platforms/tab5/components/keypad_scanner_tca8418).
//
// We poll the event FIFO from a small task rather than wiring the INT line.
// Real DOOM handles cheat codes itself, so delivering ASCII a-z / 0-9 into
// DG_GetKey is all that's needed for iddqd / idkfa / idclevXX.

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

#include "bsp/m5stack_tab5.h"
#include "doomkeys.h"
#include "kbd_tca8418.h"

static const char *TAG = "kbd_kp";

#define KP_SDA_GPIO    53          // BSP_EXT_I2C_SDA
#define KP_SCL_GPIO    54          // BSP_EXT_I2C_SCL
#define KP_I2C_PORT    0           // esp-bsp SYS bus owns port 1
#define KP_ADDR        0x34

#define REG_CFG            0x01
#define REG_INT_STAT       0x02
#define REG_KEY_LCK_EC     0x03
#define REG_KEY_EVENT_A    0x04
#define REG_GPIO_INT_STAT_1 0x11
#define REG_GPIO_INT_EN_1  0x1A
#define REG_KP_GPIO_1      0x1D
#define REG_GPI_EM_1       0x20
#define REG_GPIO_DIR_1     0x23
#define REG_GPIO_INT_LVL_1 0x26
#define CFG_KE_IEN         0x01
#define CFG_GPI_IEN        0x02

static i2c_master_dev_handle_t s_dev;

// SPSC ring (producer = poll task, consumer = doom task).
#define KPQ 64
static volatile struct { uint8_t pressed; unsigned char key; } s_q[KPQ];
static volatile uint32_t s_head, s_tail;

static void q_push(uint8_t pressed, unsigned char key)
{
    uint32_t h = s_head, n = (h + 1) & (KPQ - 1);
    if (n == s_tail) { return; }
    s_q[h].pressed = pressed;
    s_q[h].key = key;
    s_head = n;
}

bool kbd_tca8418_poll(int *pressed, unsigned char *key)
{
    uint32_t t = s_tail;
    if (t == s_head) { return false; }
    *pressed = s_q[t].pressed;
    *key = s_q[t].key;
    s_tail = (t + 1) & (KPQ - 1);
    return true;
}

static void wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    i2c_master_transmit(s_dev, b, 2, 100);
}
static uint8_t rd(uint8_t reg)
{
    uint8_t v = 0;
    i2c_master_transmit_receive(s_dev, &reg, 1, &v, 1, 100);
    return v;
}

// M5 Tab5 keyboard physical keycode (1..81) -> character. ' ' = special slot.
static const char KEYMAP[81] = {
    '0','8',':','c','k','s',  ' ',' ',' ',' ',   // 1..10   [7]=SHIFT
    '1','9',']','d','l','t',  ' ',' ',' ',' ',   // 11..20  [17]=CTRL
    '2','-',',','e','m','u',  ' ',' ',' ',' ',   // 21..30  [27]=CRPH [28]=ESC
    '3','^','.','f','n','v',  ' ',' ',' ',' ',   // 31..40  [37]=CAPS [38]=TAB
    '4','$','/','g','o','w',  ' ',' ',' ',' ',   // 41..50  [47]=KANA [48]=STOP [49]=LEFT
    '5','@','_','h','p','x',  ' ',' ',' ',' ',   // 51..60  [58]=BS [59]=UP
    '6','[','a','i','q','y',  ' ',' ',' ',' ',   // 61..70  [68]=SEL [69]=DOWN
    '7',';','b','j','r','z',  ' ',' ',' ',' ',   // 71..80  [78]=RET [79]=RIGHT
    ' ',                                          // 81      FN
};

static unsigned char map_kc(uint8_t kc)
{
    if (kc < 1 || kc > 81) { return 0; }
    switch (kc) {
    case 49: return KEY_LEFTARROW;
    case 59: return KEY_UPARROW;
    case 69: return KEY_DOWNARROW;
    case 79: return KEY_RIGHTARROW;
    case 17: case 81: return KEY_FIRE;    // CTRL, FN
    case 7:  return KEY_RSHIFT;           // SHIFT -> run
    case 47: return KEY_RALT;             // KANA  -> strafe
    case 28: return KEY_ESCAPE;
    case 38: return KEY_TAB;
    case 58: return KEY_BACKSPACE;
    case 78: return KEY_ENTER;
    case 68: return KEY_USE;              // SEL -> use/open
    default: break;
    }
    char c = KEYMAP[kc - 1];
    if (c > ' ' && c < 127) { return (unsigned char)c; }  // a-z / 0-9 / symbols
    return 0;
}

static void kp_task(void *arg)
{
    (void)arg;
    // init: all key/GPI pins as inputs, key-event mode, then claim the matrix.
    for (int i = 0; i < 3; i++) {
        wr(REG_GPIO_DIR_1 + i, 0x00);
        wr(REG_GPI_EM_1 + i, 0xFF);
        wr(REG_GPIO_INT_LVL_1 + i, 0x00);
        wr(REG_GPIO_INT_EN_1 + i, 0xFF);
    }
    wr(REG_KP_GPIO_1, 0xFF);   // rows 0..7
    wr(REG_KP_GPIO_1 + 1, 0xFF); // cols 0..7
    wr(REG_KP_GPIO_1 + 2, 0x01); // col 8
    wr(REG_CFG, rd(REG_CFG) | CFG_KE_IEN | CFG_GPI_IEN);

    // flush any stale events
    while (rd(REG_KEY_EVENT_A) != 0) { }
    for (int i = 0; i < 3; i++) { rd(REG_GPIO_INT_STAT_1 + i); }
    wr(REG_INT_STAT, 0x03);

    ESP_LOGI(TAG, "TCA8418 keypad ready");

    for (;;) {
        int pending = rd(REG_KEY_LCK_EC) & 0x0F;
        while (pending-- > 0) {
            uint8_t ev = rd(REG_KEY_EVENT_A);
            if (ev == 0) { break; }
            uint8_t kc = ev & 0x7F;
            unsigned char k = map_kc(kc);
            if (k) { q_push((ev & 0x80) ? 1 : 0, k); }
        }
        wr(REG_INT_STAT, 0x03);
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

void kbd_tca8418_start(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = KP_I2C_PORT,
        .sda_io_num = KP_SDA_GPIO,
        .scl_io_num = KP_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    esp_err_t e = i2c_new_master_bus(&bus_cfg, &bus);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus (EXT, sda%d/scl%d) -> %s",
                 KP_SDA_GPIO, KP_SCL_GPIO, esp_err_to_name(e));
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = KP_ADDR,
        .scl_speed_hz = 400000,
    };
    e = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "add_device 0x%02x -> %s", KP_ADDR, esp_err_to_name(e));
        return;
    }
    // one-shot scan of BOTH buses so we can see where the keyboard actually is
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(bus, a, 40) == ESP_OK) {
            ESP_LOGW(TAG, "EXT bus (sda%d/scl%d): device @0x%02x", KP_SDA_GPIO, KP_SCL_GPIO, a);
            found++;
        }
    }
    i2c_master_bus_handle_t sys = NULL;
    if (i2c_master_get_bus_handle(BSP_I2C_NUM, &sys) == ESP_OK && sys) {
        for (uint8_t a = 0x08; a < 0x78; a++) {
            if (i2c_master_probe(sys, a, 40) == ESP_OK) {
                ESP_LOGW(TAG, "SYS bus (port%d, sda31/scl32): device @0x%02x", BSP_I2C_NUM, a);
                found++;
            }
        }
    }
    if (!found) {
        ESP_LOGW(TAG, "no I2C devices on either bus - keyboard not plugged in?");
    }

    xTaskCreatePinnedToCore(kp_task, "kp", 3072, NULL, 4, NULL, 0);
}
