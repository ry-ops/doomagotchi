# doom-node — DOOMAGOTCHI renderer (M5Stack Tab5 / ESP32-P4)

Runs **unmodified** `doomgeneric` (vendored at `third_party/doomgeneric/`, ADR 0001).
Our code is `main/doomgeneric_esp32p4.c` (the six `DG_*` functions) plus CMake glue.

## Build

```sh
git submodule update --init --recursive        # from repo root
. $IDF_PATH/export.sh                           # ESP-IDF v6.1+
cd doom-node
idf.py set-target esp32p4
idf.py build
idf.py size            # <- Phase 0 deliverable: DOOM's static footprint
```

## Phase 0 status

- [x] `doomgeneric` vendored as submodule, unmodified
- [x] ESP-IDF project builds and links against it (`app_main` pins the engine in)
- [ ] Freedoom IWAD on SD, loaded via a `DG_`/VFS file shim
- [ ] `DG_DrawFrame` blits `DG_ScreenBuffer` to the MIPI-DSI panel
- [x] `DG_GetTicksMs` / `DG_SleepMs` wired to FreeRTOS / `esp_timer`
- [ ] E1M1 renders, playable, stable framerate

The engine is linked but not invoked yet — see the comment in `main/app_main.c`.
