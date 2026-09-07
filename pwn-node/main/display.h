#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "mood.h"

// ST7789 240x135 status display for the DOOMAGOTCHI pwn node (Cardputer Adv).
//
// Not a DOOM renderer -- the Tab5 does that. This is the sensor's face: a
// DOOM-marine mug that reacts to the mood machine, plus a compact status bar
// (channel / APs / handshakes / PMKIDs / intensity). Renders into an in-RAM
// RGB565 framebuffer and blits the whole thing a couple of times a second.
//
// If the panel can't be brought up, display_start() returns false and
// display_render() is a no-op -- the sensor runs on headless.

typedef struct {
    uint8_t  channel;
    uint32_t aps;
    uint32_t handshakes;
    uint32_t pmkids;
    uint32_t uptime_s;
    const char *last_enemy;   // name of the last AP's enemy class, or NULL
    mood_state_t mood;
} disp_model_t;

bool display_start(void);
void display_render(const disp_model_t *m);
