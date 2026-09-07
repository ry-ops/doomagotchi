# doom-node — DOOMAGOTCHI renderer (M5Stack Tab5 / ESP32-P4)

Runs **unmodified** `doomgeneric` (vendored at `third_party/doomgeneric/`, ADR 0001).
Our code is `main/doomgeneric_esp32p4.c` (the six `DG_*` functions) plus glue.

## Build & flash

```sh
git submodule update --init --recursive        # from repo root
. $IDF_PATH/export.sh                           # ESP-IDF v6.1+
cd doom-node
idf.py set-target esp32p4
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

First boot with no `/sdcard/freedoom1.wad`: the firmware enters "WAD receive
mode" — run `python ../tools/send-wad.py /dev/cu.usbmodem1101 ../assets/freedoom1.wad`
(needs pyserial), it writes the IWAD to the SD over the console and reboots.

## Phases 0 & 1 — DONE ✅

**Gate: Freedoom is playable on the Tab5 by hand.**

- [x] `doomgeneric` vendored as submodule, unmodified
- [x] ESP-IDF project builds and links (esp32p4, chip rev v1.3 @ 360 MHz)
- [x] Freedoom IWAD on SD, loaded via `fopen` on `/sdcard/freedoom1.wad`
- [x] `DG_DrawFrame`: DPI framebuffer + PPA HW rotate(90 CCW)/scale/XRGB→RGB565,
      full-screen 720×1280, ~19 ms/frame
- [x] `DG_GetTicksMs` / `DG_SleepMs` → `esp_timer` / FreeRTOS
- [x] `DG_GetKey`: three sources merged —
      touch zones (GT911/ST7123), USB HID keyboard (USB-A host port), and the
      **official M5 Tab5 Keyboard (A164)**: STM32 keypad @ I2C `0x6D` on
      GPIO 0/1, driven in HID mode. DOOM's own cheat responder means
      `iddqd` / `idkfa` / `idclevXX` work with no extra code.
- [x] E1M1 renders, playable (~15 tics/s end to end), stable, no leak —
      first level completed by hand on the keyboard

The A164's event stream is one-key-at-a-time (release = HID usage 0), so
forward+turn can't be held together. Fine for now; Phase 1's autoplayer
doesn't use `DG_GetKey`.

### Phase 1 — the autoplayer (`main/autoplay.c`)

`autoplay_step()` runs once per frame before `doomgeneric_Tick()`. All output
is `D_PostEvent` keypresses (`KEY_UP/DOWN/LEFT/RIGHT`, `KEY_STRAFE_L/R`,
`KEY_FIRE`, `KEY_USE`, weapon digits `1`–`7`) — the human key path, zero engine
edits.

Each frame, in order:

1. **acquire** — one thinker-list pass for the cheapest monster, where *cost* =
   2-D distance + `4 ×` height gap. The `4×` Z weight keeps it on same-floor
   targets and off monsters stuck down a pit until they're all that's left. The
   lock is re-validated against the list every frame (no dangling mobj) and
   held unless something is clearly cheaper.
2. **shoot** — sweep DOOM's own autoaim trace (`P_AimLineAttack`) across ±54°.
   A hit is a real unobstructed shot (the trace stops at walls), so this is the
   fire gate — `P_CheckSight` is unusable in the Freedoom IWAD (its REJECT lump
   rejects almost everything). On a hit: turn onto it, pick the weapon for its
   `mobjtype` (`weap_for()` — pistol/shotgun/chaingun/plasma/rockets/BFG, close
   range downgrades rockets/BFG), hold the trigger, creep to the standoff.
3. **drop** — target ≥56 units below and near in 2-D → we're on a pit rim;
   walk straight off it (don't treat the rim as a door), sliding along if a
   rail blocks the first spot.
4. **navigate** — route to the target through the sector graph (`nav.c`, below).
   The waypoint is the next doorway; steer at it, hold forward through a wide
   cone, and lean on `USE` when the graph says the crossing is a door/lift.
   No route (same room / unreachable) → steer straight at the monster and let
   DOOM's wall-slide carry the glancing contact; `pick_dir()` nudges the aim
   around a grazed corner (read-only `P_CheckPosition` probes).
5. **wedge kick** — genuinely not moving → short alternating turn+strafe+fwd
   burst, `USE` tapped fast. No RNG.
6. **give up** — ~8 s with no ground gained (or stuck at a pit rim for ~3 s, or
   2 failed kicks) → blacklist the target ~10 s and let `acquire` pick another.

**`nav.c` — the level graph.** Built once per level (rebuilt on `E?M?` change),
all read-only queries into `sectors[]` / `lines[]` / `R_PointInSubsector` — no
engine edits. Nodes are sectors (centre = `soundorg`); an edge is a two-sided
linedef the player can cross: opening ≥ 56, step-up ≤ 24 *or* the line has a
special (door/lift — passable after a `USE`, cost-penalised). A* (heuristic =
centre-to-centre distance) from the player's sector to the target's; the result
is a list of doorway midpoints to walk. E1M1: 182 sectors, 942 edges, ~32 KiB
PSRAM, sub-ms to build.

**Human control.** Any physical keypress (`DG_GetKey`) suspends the autoplayer
for 5 s — it releases every key and posts nothing, so you can type cheats
(`idclip`, `iddqd`, …) or steer manually with a clean channel, no fighting the
bot and no stomped cheat sequences. Resumes on its own once the keyboard is
quiet. Cheats are also re-entered by the bot on every level start (+ periodic
`idfa`).

**Keepalive spawn** (`KEEPALIVE_SPAWN`, on). No kill in ~30 s → `P_SpawnMobj` an
imp ~220 units in front of the player. Stand-in for the Phase 3 RF feed (same
spawn call `AP_SEEN` will make); flip the define off once the LoRa RX lands.

- [x] `ticcmd` generator replaces human input; hunts + **kills** (autoaim-gated fire)
- [x] a different weapon per monster type
- [x] routes through the level via an A* sector graph (doors/lifts/stairs)
- [x] Z-aware target choice + walk-off-the-ledge for monsters below
- [x] human keypress → autoplayer stands down for clean manual / cheat input
- [x] keepalive imp spawn when nothing's reachable
- Hardware (Tab5, 2026-09-07): ~12 kills / 2 min on E1M1, weapon switches
  correct, recovers from every stuck spot. Fully walled pits still need the
  keepalive / RF feed rather than a path. Boot fault not seen again.

Runtime footprint: 6.00 MiB zone + 1.00 MiB `DG_ScreenBuffer` + ~1.8 MiB DPI
framebuffer, all PSRAM (23 MiB free). Internal SRAM ~170 KiB free.

## Notes / follow-ups

- `-warp 1 1` in `app_main.c` jumps straight to E1M1 (skips title/demo).
- `esp_lcd_touch_get_coordinates` is deprecated (removed in touch comp 2.0);
  swap for `esp_lcd_touch_get_data` on a later pass.
- `idf.py build` sometimes doesn't notice edits — `touch main/<file>.c` first.
- Framerate: `doomgeneric` is built `-Os`; `-O2` + PPA non-blocking are the
  obvious wins if Phase 1's autoplayer wants it smoother.
