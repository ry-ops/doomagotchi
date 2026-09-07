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

`autoplay_step()` runs once per frame before `doomgeneric_Tick()`. It reads
the player mobj + the thinker list, finds the nearest `MF_COUNTKILL` monster
it has `P_CheckSight` to, turns toward it (`KEY_LEFT`/`RIGHT`), fires within
~12°, closes to a standoff. No visible target → wander. Blocked path (dist
not improving, or no movement while holding forward) → back off + hard turn.
Types `iddqd`+`idkfa` on start, `idfa` every ~3000 frames. All via
`D_PostEvent` — the human key path, zero engine edits.

- [x] `ticcmd` generator replaces human input
- [x] wander / face nearest monster / fire, reusing DOOM's AI
- [x] no pathfinder — just LOS + a give-up timer
- [x] **7h45m unattended on hardware, 0 crashes, never permanently stuck**

Runtime footprint: 6.00 MiB zone + 1.00 MiB `DG_ScreenBuffer` + ~1.8 MiB DPI
framebuffer, all PSRAM (23 MiB free). Internal SRAM ~170 KiB free.

## Notes / follow-ups

- `-warp 1 1` in `app_main.c` jumps straight to E1M1 (skips title/demo).
- `esp_lcd_touch_get_coordinates` is deprecated (removed in touch comp 2.0);
  swap for `esp_lcd_touch_get_data` on a later pass.
- `idf.py build` sometimes doesn't notice edits — `touch main/<file>.c` first.
- Framerate: `doomgeneric` is built `-Os`; `-O2` + PPA non-blocking are the
  obvious wins if Phase 1's autoplayer wants it smoother.
