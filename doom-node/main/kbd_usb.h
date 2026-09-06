#pragma once
#include <stdbool.h>

// Bring up the USB HID keyboard on the Tab5's OTG host port.
void kbd_usb_start(void);

// Pop one translated key event (doomgeneric DG_GetKey semantics: *pressed
// 1=down/0=up, *key = DOOM keycode). Returns false when the queue is empty.
bool kbd_usb_poll(int *pressed, unsigned char *key);

// USB HID keyboard usage id -> DOOM keycode (0 if unmapped). Shared with the
// M5 pin-connector keyboard, which also reports HID usage codes.
unsigned char kbd_hid_usage_to_doom(unsigned char usage);
