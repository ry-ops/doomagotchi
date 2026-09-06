# ADR 0001 — Vendor `doomgeneric`; do not write a renderer

- **Status:** Accepted
- **Date:** 2026-09-06
- **Deciders:** project owner
- **Supersedes:** every prior attempt at this project

## Context

The goal is a *faithful* DOOM whose world is driven by the live RF environment.
"Faithful" is load-bearing: the payoff of the concept — real E1M1 geometry, real
imp sprites, the real intermission screen — only lands if it is actually DOOM, not
something that resembles it.

Every previous run at this project died the same way. It started with "I'll write a
small raycaster / a minimal BSP renderer / a DOOM-like, just to get pixels on the
screen," and then spent all its time tuning magic constants to approximate a look
that a 1993 codebase already produces exactly. The renderer is a tar pit. It is
weeks of work to get 70% of the way to something that a free, MIT/GPL, purpose-built
port gives us at 100% on day one.

`doomgeneric` (ozkl/doomgeneric, Chocolate-Doom lineage) exists specifically to be
ported to new platforms. It isolates everything platform-specific into six
functions:

```c
void     DG_Init(void);
void     DG_DrawFrame(void);
void     DG_SleepMs(uint32_t ms);
uint32_t DG_GetTicksMs(void);
int      DG_GetKey(int *pressed, unsigned char *key);
void     DG_SetWindowTitle(const char *title);
```

Everything above that line — `p_*`, `r_*`, `g_*`, `m_*`, `w_*`, the zone allocator,
fixed-point math, the tic loop, WAD parsing, the state machine in `info.c` — is the
real engine and is none of our business.

## Decision

1. Vendor `doomgeneric` as a git submodule at `doom-node/third_party/doomgeneric/`,
   pinned to a specific commit. Initial pin: `dcb7a8d`.
2. Treat that directory as **read-only**. No patches, no `#ifdef DOOMAGOTCHI`, no
   "just one small fix." If upstream has a bug that blocks us, we carry the fix as a
   documented patch file applied at build time and open the issue upstream — we do
   not edit the working tree.
3. Our entire contribution on the DOOM side is one file — `doom-node/main/
   doomgeneric_esp32p4.c` — implementing the six `DG_*` functions, plus the
   ESP-IDF component/CMake glue and a compat shim for the handful of libc symbols
   bare-metal newlib lacks (`system()`).
4. The RF layer reaches DOOM only through interfaces the engine already exposes to a
   second player: the monster-spawn path and the player `ticcmd`. See ADR 0003.
5. Any proposal that requires editing a file under `p_`, `r_`, `g_`, `m_`, or `w_`
   is rejected by default and must be escalated to the project owner with the
   specific reason the `DG_*` seam and the spawn/`ticcmd` seam are both insufficient.

## Consequences

**Positive**

- The renderer is done. Zero time spent approximating DOOM's look.
- Upstream fixes and the broader doomgeneric porting community are ours for free.
- A crisp, tiny surface to test and review: six functions plus glue.
- Re-pinning the submodule is a deliberate, reviewable event.

**Negative / accepted costs**

- We inherit ~80 C files of 1993-vintage code and must make a modern cross-compiler
  (GCC 15, `riscv32-esp-elf`) accept them. Mitigation: warning-suppression flags on
  the vendored component only; our own code stays `-Werror`-clean.
- We inherit DOOM's memory model: the zone allocator wants multiple megabytes in one
  contiguous block. This is *why* the DOOM node is the ESP32-P4 with 32MB PSRAM
  (ADR 0002), and why Phase 0's first deliverable is a memory map, not a display
  driver — we size the zone against real linker numbers before building anything on
  top.
- We cannot cherry-pick engine behaviour. If we want the autoplayer to influence the
  game, it does so as a player would, or not at all.

## Notes for future temptation

If you are reading this because you just thought "it would be so much easier to just
tweak `P_SpawnMobj` / `R_DrawColumn` / `G_BuildTiccmd`": that is the exact thought
that killed the last three attempts. The seam is the six `DG_*` functions and the
spawn/`ticcmd` injection points. Work within it or bring the owner a reason it
genuinely cannot be done there.
