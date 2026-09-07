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
