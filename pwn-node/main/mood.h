#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "dgm_event.h"

// The pwn node's personality. It's a DOOM marine, not a pet: an *inverted*
// pwnagotchi. A normal pwnagotchi is content when it's catching handshakes and
// sad when the air is quiet. This one is the opposite of content -- a busy
// airspace makes it manic and bloodthirsty; dead air makes it bored and mean.
//
// Pure logic, no hardware. Feed it every dgm_event and a 1 Hz tick; read back
// a mood + intensity + a one-line quip for the ST7789 face.

typedef enum {
    MOOD_BORED = 0,    // dead air, nothing for minutes
    MOOD_RESTLESS,     // the odd beacon, no blood
    MOOD_HUNTING,      // steady new APs, locked in
    MOOD_MANIC,        // a handshake/PMKID landed recently
    MOOD_RAMPAGE,      // multiple kills in a short window
    MOOD_COUNT,
} mood_t;

typedef struct {
    mood_t      mood;
    uint8_t     intensity;   // 0..255, drives face animation / blink rate
    const char *label;       // "RAMPAGE"
    const char *quip;        // rotating one-liner for the status bar
    uint32_t    aps_1m;      // new APs in the last 60 s
    uint32_t    caps_2m;     // handshakes+PMKIDs in the last 120 s
} mood_state_t;

void mood_init(void);
void mood_on_event(const struct dgm_event *ev);
void mood_tick(void);                 // call ~1 Hz
void mood_get(mood_state_t *out);
