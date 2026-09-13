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

// Meta hotkeys handled entirely at the input layer, never forwarded into the
// DOOM engine as a keycode: TILDE toggles the autoplayer on/off, DELETE
// restarts the current level (see autoplay.h). Bound to these two because the
// M5 Tab5 keyboard has no F-row and every letter/digit is already claimed for
// cheat-code typing. Call on every raw HID usage transition,
// before kbd_hid_usage_to_doom() - returns true if this usage was a meta key
// (already actioned on the press edge; the release edge is swallowed too), in
// which case the caller should not translate/queue it as a game key.
bool kbd_hid_usage_meta(unsigned char usage, bool pressed);
