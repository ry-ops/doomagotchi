#pragma once
#include <stdbool.h>

// Official M5Stack Tab5 pin-connector keyboard (TCA8418 on the EXT I2C bus).
void kbd_tca8418_start(void);

// Pop one translated key event (DG_GetKey semantics). False when empty.
bool kbd_tca8418_poll(int *pressed, unsigned char *key);
