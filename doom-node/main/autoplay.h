#pragma once

// Phase 1 autoplayer. Call once per frame, before doomgeneric_Tick(). It reads
// game state (player mobj + the thinker list) and drives the player by posting
// synthetic keyboard events through D_PostEvent — the same path a human's keys
// take. It never touches game logic (ADR 0001 / CLAUDE.md constraint #3).
void autoplay_step(void);
