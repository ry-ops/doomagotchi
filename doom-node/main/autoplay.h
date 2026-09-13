#pragma once

#include <stdbool.h>

// The autoplayer. Call once per frame, before doomgeneric_Tick(). It reads game
// state (player mobj, thinker list, and the read-only sector/line graph via
// nav.c) and drives the player by posting synthetic keyboard events through
// D_PostEvent — the same path a human's keys take. It never touches game logic
// (ADR 0001 / CLAUDE.md constraint #3).
void autoplay_step(void);

// Call from the real-keyboard path (DG_GetKey) on every physical keypress. The
// autoplayer then stands down for a few seconds — releases everything and posts
// nothing — so a human can type cheat codes or drive manually without the bot
// fighting the input or stomping a half-typed cheat sequence.
void autoplay_note_human_key(void);

// True while that hold-off is active (autoplay_step is a no-op).
bool autoplay_suspended(void);

// Explicit on/off latch, independent of the standdown timer above - a hard
// override so the bot stays out of the way for as long as a human wants,
// rather than clawing control back 5s after the last keypress. Flip it from a
// dedicated meta key (TILDE); see kbd_usb.c / kbd_m5kbd.c.
void autoplay_toggle_enabled(void);
bool autoplay_is_enabled(void);

// One-shot: type "idclev<episode><map>" for the level DOOM is on right now,
// restarting it fresh (same mechanism as the iddqd/idkfa auto-cheats - never
// touches game logic directly, per ADR 0001). Bound to a dedicated meta key
// (the DELETE key). Runs unconditionally, independent of autoplay_is_enabled() /
// autoplay_suspended(), so the hotkey works whoever currently has control.
void autoplay_request_restart(void);

// Who currently has the wheel, for the on-screen indicator in
// doomgeneric_esp32p4.c. HUMAN covers both "explicitly disabled" and "standing
// down after a real keypress"; AUTO is everything else.
typedef enum { AUTOPLAY_DRIVER_AUTO, AUTOPLAY_DRIVER_HUMAN } autoplay_driver_t;
autoplay_driver_t autoplay_current_driver(void);
