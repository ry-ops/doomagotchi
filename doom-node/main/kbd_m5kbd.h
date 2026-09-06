#pragma once
#include <stdbool.h>

// Official M5Stack Tab5 Keyboard (SKU A164): STM32F030 keypad controller,
// I2C addr 0x6D on the Tab5 keyboard bus (SDA=GPIO0, SCL=GPIO1, INT=GPIO50).
// Driven in HID mode - reg 0x30 reports [modifier][usage] like a USB keyboard.
void kbd_m5kbd_start(void);

// Pop one translated key event (DG_GetKey semantics). False when empty.
bool kbd_m5kbd_poll(int *pressed, unsigned char *key);
