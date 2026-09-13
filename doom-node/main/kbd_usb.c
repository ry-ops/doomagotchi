// kbd_usb.c — USB HID keyboard on the Tab5's OTG host port -> DOOM key events.
//
// Real DOOM handles cheat codes itself (st_stuff.c ST_Responder watches the
// keydown stream for "iddqd", "idkfa", "idclevXX", ...), so all this needs to
// do is deliver ASCII letters/digits and the movement keys into DG_GetKey.

#include <string.h>
#include <stdbool.h>

#include "esp_log.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"

#include "bsp/m5stack_tab5.h"
#include "doomkeys.h"
#include "kbd_usb.h"
#include "autoplay.h"

static const char *TAG = "kbd";

// SPSC ring: producer = HID background task, consumer = the doom task.
#define KBQ 64
static volatile struct { uint8_t pressed; unsigned char key; } s_q[KBQ];
static volatile uint32_t s_head, s_tail;

static void q_push(uint8_t pressed, unsigned char key)
{
    uint32_t h = s_head;
    uint32_t n = (h + 1) & (KBQ - 1);
    if (n == s_tail) {
        return; // full - drop
    }
    s_q[h].pressed = pressed;
    s_q[h].key = key;
    s_head = n;
}

bool kbd_usb_poll(int *pressed, unsigned char *key)
{
    uint32_t t = s_tail;
    if (t == s_head) {
        return false;
    }
    *pressed = s_q[t].pressed;
    *key = s_q[t].key;
    s_tail = (t + 1) & (KBQ - 1);
    return true;
}

// HID keyboard usage id -> DOOM key. Letters/digits stay ASCII (lowercase) so
// the engine's cheat/menu responders see them directly.
unsigned char kbd_hid_usage_to_doom(unsigned char uc)
{
    if (uc >= HID_KEY_A && uc <= HID_KEY_Z) {
        return (unsigned char)('a' + (uc - HID_KEY_A));
    }
    if (uc >= HID_KEY_1 && uc <= HID_KEY_9) {
        return (unsigned char)('1' + (uc - HID_KEY_1));
    }
    switch (uc) {
    case HID_KEY_0:          return '0';
    case HID_KEY_ENTER:
    case HID_KEY_KEYPAD_ENTER: return KEY_ENTER;
    case HID_KEY_ESC:        return KEY_ESCAPE;
    case HID_KEY_DEL:        return KEY_BACKSPACE;
    case HID_KEY_TAB:        return KEY_TAB;
    case HID_KEY_SPACE:      return KEY_USE;
    case HID_KEY_MINUS:      return KEY_MINUS;
    case HID_KEY_EQUAL:      return KEY_EQUALS;
    case HID_KEY_RIGHT:      return KEY_RIGHTARROW;
    case HID_KEY_LEFT:       return KEY_LEFTARROW;
    case HID_KEY_DOWN:       return KEY_DOWNARROW;
    case HID_KEY_UP:         return KEY_UPARROW;
    // hid_usage_keyboard.h gives RIGHT_* the same values as LEFT_*, so match the
    // real USB HID codes for the right-hand modifiers (0xE4..0xE6) by hand.
    case HID_KEY_LEFT_CONTROL: case 0xE4: return KEY_FIRE;
    case HID_KEY_LEFT_SHIFT:   case 0xE5: return KEY_RSHIFT;  // run
    case HID_KEY_LEFT_ALT:     case 0xE6: return KEY_RALT;    // strafe
    default:                              return 0;
    }
}

// The M5 Tab5 keyboard (A164) has no F-row, so the meta keys live on two
// symbols kbd_hid_usage_to_doom() never maps to anything - confirmed free by
// probing raw HID usage bytes over serial rather than guessing from a spec
// sheet: TILDE (0x35) and the dedicated forward-DELETE key (0x4C, distinct
// from HID_KEY_DEL/0x2A, which this SDK's naming actually uses for Backspace).
bool kbd_hid_usage_meta(unsigned char uc, bool pressed)
{
    if (uc != HID_KEY_TILDE && uc != HID_KEY_DELETE) {
        return false;
    }
    if (pressed) {
        if (uc == HID_KEY_TILDE) {
            autoplay_toggle_enabled();
        } else {
            autoplay_request_restart();
        }
    }
    return true;   // swallow the release edge too - never reaches the engine
}

static void iface_cb(hid_host_device_handle_t dh,
                     const hid_host_interface_event_t event, void *arg)
{
    (void)arg;
    if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "keyboard disconnected");
        hid_host_device_close(dh);
        return;
    }
    if (event != HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        return;
    }

    uint8_t data[16];
    size_t len = 0;
    if (hid_host_device_get_raw_input_report_data(dh, data, sizeof(data), &len) != ESP_OK) {
        return;
    }
    if (len < 8) {
        return; // boot keyboard report is [mods][rsvd][6 keycodes]
    }

    static uint8_t prev[6];
    const uint8_t *cur = &data[2];

    for (int i = 0; i < 6; i++) {
        if (prev[i] > HID_KEY_ERROR_UNDEFINED && !memchr(cur, prev[i], 6)) {
            if (kbd_hid_usage_meta(prev[i], false)) { continue; }
            unsigned char k = kbd_hid_usage_to_doom(prev[i]);
            if (k) { q_push(0, k); }
        }
    }
    for (int i = 0; i < 6; i++) {
        if (cur[i] > HID_KEY_ERROR_UNDEFINED && !memchr(prev, cur[i], 6)) {
            if (kbd_hid_usage_meta(cur[i], true)) { continue; }
            unsigned char k = kbd_hid_usage_to_doom(cur[i]);
            if (k) { q_push(1, k); }
        }
    }
    memcpy(prev, cur, 6);
}

static void dev_cb(hid_host_device_handle_t dh,
                   const hid_host_driver_event_t event, void *arg)
{
    (void)arg;
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) {
        return;
    }
    hid_host_dev_params_t p;
    if (hid_host_device_get_params(dh, &p) != ESP_OK) {
        return;
    }
    ESP_LOGI(TAG, "HID connected: sub_class=%d proto=%d", p.sub_class, p.proto);

    const hid_host_device_config_t cfg = { .callback = iface_cb, .callback_arg = NULL };
    if (hid_host_device_open(dh, &cfg) != ESP_OK) {
        return;
    }
    if (p.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        hid_class_request_set_protocol(dh, HID_REPORT_PROTOCOL_BOOT);
        if (p.proto == HID_PROTOCOL_KEYBOARD) {
            hid_class_request_set_idle(dh, 0, 0);
        }
    }
    hid_host_device_start(dh);
}

void kbd_usb_start(void)
{
    esp_err_t e = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true);
    ESP_LOGI(TAG, "bsp_usb_host_start -> %s", esp_err_to_name(e));
    if (e != ESP_OK) {
        return;
    }

    const hid_host_driver_config_t cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = dev_cb,
        .callback_arg = NULL,
    };
    e = hid_host_install(&cfg);
    ESP_LOGI(TAG, "hid_host_install -> %s", esp_err_to_name(e));
}
