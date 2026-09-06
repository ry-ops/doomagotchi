# DOOMAGOTCHI — Project Kickoff Brief

## What we are building

A passive, self-playing DOOM that renders the live 2.4GHz RF environment as demons.

Nobody plays it. It plays itself, like a pwnagotchi. You carry a sensor around; nearby
access points spawn as monsters; captured handshakes are kills; the end-of-session tally
is a DOOM intermission screen.

Two devices, connected over sub-GHz LoRa.

---

## Non-negotiable constraints

Read these before writing any code. Violating any one of them means starting over.

### 1. DO NOT WRITE A DOOM ENGINE.

This is the single most important rule in this document.

You will be tempted to write a raycaster, a "DOOM-like," a simplified BSP renderer, or a
"minimal DOOM implementation." **Do not.** Every one of those is a failure state. Previous
attempts at this project died exactly here.

We are running **actual DOOM**. Vendor [`doomgeneric`](https://github.com/ozkl/doomgeneric)
into `doom-node/third_party/doomgeneric/` as a git submodule and treat that directory as
**read-only**.

`doomgeneric` exists specifically to be ported. It factors the platform layer down to a
handful of functions:

```c
void DG_Init(void);
void DG_DrawFrame(void);
void DG_SleepMs(uint32_t ms);
uint32_t DG_GetTicksMs(void);
int  DG_GetKey(int* pressed, unsigned char* key);
void DG_SetWindowTitle(const char* title);
```

Your entire job on the DOOM side is implementing those six functions for our hardware.
Everything above them — P_*, R_*, G_*, WAD loading, the zone allocator, fixed-point math,
the tic loop — stays byte-identical to upstream. If you find yourself editing anything
under `p_`, `r_`, `g_`, `m_`, or `w_`, stop and ask.

This constraint is what guarantees a 100% faithful DOOM. We are not cloning it. We are
running it.

### 2. Assets are Freedoom, not id Software.

Use [Freedoom](https://freedoom.github.io/) Phase 1 IWAD. Same map layout, same sprite
silhouettes, same engine compatibility, freely licensed. This keeps the repo publishable.
Never commit or reference a commercial `DOOM.WAD`.

### 3. Game logic is never modified to accommodate RF.

The RF layer talks to DOOM by **injecting spawn events into the existing monster spawn
path** and by driving the player actor's `ticcmd` — the same input struct a human joystick
would fill. Nothing else. If a feature seems to require patching game logic, it's the wrong
feature.

---

## Hardware architecture

Two devices. Each one covers a disqualifying weakness in the other. This split is forced by
hardware reality, not preference.

### Tab5 — the renderer ("DOOM node")

- ESP32-P4, RISC-V dual-core @ 360MHz, 16MB flash, **32MB PSRAM**
- 5" 1280×720 IPS via MIPI-DSI
- ESP32-C6 wireless co-processor over SDIO

**Why it runs DOOM:** the 32MB of PSRAM. DOOM's zone allocator needs megabytes. This is the
only device in the build that has them.

**Why it does NOT do the RF capture:** the P4 has no radio of its own. WiFi lives on a
separate C6 talking over SDIO, so the P4 never sees raw 802.11 frames. Getting promiscuous
mode across that bus means writing custom hosted-firmware for the C6 — a large detour we are
deliberately routing around.

### Cardputer Adv — the sensor ("pwn node")

- ESP32-S3FN8 @ 240MHz, 8MB flash, **no PSRAM**
- 240×135 ST7789V2 LCD, 56-key keyboard, BMI270 IMU
- 1750mAh battery, microSD, ES8311 audio
- LoRa 1262 Cap (SX1262, sub-GHz)

**Why it does the RF capture:** `esp_wifi_set_promiscuous()` is a first-class ESP-IDF call on
this exact silicon. It just works. Pin the radio and channel-hop timer to core 0, everything
else to core 1.

**Why it does NOT run DOOM:** no PSRAM. 512KB of internal SRAM cannot hold DOOM. Do not
attempt it, and do not propose a cut-down DOOM to make it fit — see constraint #1.

Its screen runs the **status bar only**: the reactive face, kill counter, ammo (= handshakes),
health (= battery), current channel. That's a genuinely great use of a 240×135 panel.

### The seam — LoRa SX1262

Sub-GHz, so it shares no band, antenna, or duty cycle with the 2.4GHz capture radio. Zero
contention. Range measured in kilometers, so the Tab5 can sit at home while the Cardputer
walks around town.

**Confirm the Cap is the 915MHz variant before transmitting.** M5 ships both 868 (EU) and 915
(US ISM). Bench-test only if it's an 868 unit.

---

## The protocol contract — define this first

Both halves get built against this. Write it before either device.

Low bandwidth is a feature here, not a limitation. Original DOOM netcode exchanged a few
bytes of `ticcmd` per player per tic over 1993 serial links. We're in similar territory.

**We do not sync game state.** Each node simulates locally. LoRa carries events only.

```
struct dgm_event {
    uint8_t  version;      // protocol rev
    uint8_t  unit_id;      // which sensor
    uint8_t  event_type;   // AP_SEEN | HANDSHAKE | PMKID | AP_LOST | SESSION_END
    uint8_t  enemy_class;  // derived from encryption suite
    uint8_t  rssi_bucket;  // 0-7, maps to spawn distance
    uint8_t  bssid_hash;   // stable 8-bit id, dedupe only
} __attribute__((packed));   // 6 bytes
```

Enemy class mapping (initial pass, tune later):

| RF observation      | Enemy          |
|---------------------|----------------|
| Open network        | Zombieman      |
| WEP                 | Shotgun Guy    |
| WPA2-PSK            | Imp            |
| WPA3-SAE            | Cacodemon      |
| Hidden SSID         | Spectre        |
| 802.1X / Enterprise | Baron of Hell  |

`HANDSHAKE` = full four-way capture. `PMKID` = the fast kill. Both resolve to a monster death
on the DOOM node; the difference is which weapon the autoplayer uses.

---

## Phase plan — hard gates, no skipping

### Phase 0 — DOOM boots on Tab5. Nothing else exists.

No RF. No LoRa. No pwnagotchi anything. Do not write a single line of WiFi code during
this phase.

- [ ] `doomgeneric` vendored as submodule, unmodified
- [ ] ESP-IDF project builds and links against it
- [ ] Freedoom IWAD on SD card, loads via a `DG_` file shim
- [ ] `DG_DrawFrame` blits DOOM's 320×200 framebuffer to the MIPI-DSI panel
- [ ] `DG_GetTicksMs` / `DG_SleepMs` wired to FreeRTOS
- [ ] E1M1 renders, playable via touch or USB keyboard, stable framerate

**Gate: you can play Freedoom on the Tab5 by hand.** Until this is green, the project does not
have a second phase. This is 80% of the risk in the whole build and it is all front-loaded
here on purpose.

### Phase 1 — the autoplayer

- [ ] Replace human input with a `ticcmd` generator
- [ ] Behavior: wander, turn toward nearest live monster, fire, repeat
- [ ] Reuse DOOM's existing monster AI states — do not invent a pathfinder
- [ ] Runs unattended for an hour without getting stuck on a wall

### Phase 2 — the sensor, standalone

Separate ESP-IDF project. Builds and runs with no Tab5 present.

- [ ] Promiscuous mode + channel hop timer pinned to core 0
- [ ] EAPOL frame filter, four-way handshake and PMKID detection
- [ ] pcap writer to SD card (file extension: `.WAD`, non-negotiable)
- [ ] DOOM status bar on the 240×135 LCD with a reactive face
- [ ] Mood state machine — inverted pwnagotchi: bored/annoyed on a dead channel,
      manic on a good haul
- [ ] Serial-prints `dgm_event` structs it would have transmitted

### Phase 3 — join the seam

- [ ] SX1262 driver up on the Cardputer, TX only
- [ ] C6-side or USB receive path on the Tab5
- [ ] Events dedupe by `bssid_hash`, spawn monsters at `rssi_bucket` distance
- [ ] End-to-end: walk past a router, watch an imp appear on the Tab5

### Phase 4 — polish

- [ ] Intermission screen from real session stats (KILLS %, SECRETS, TIME)
- [ ] Level naming from GPS if a Grove GPS unit is attached
- [ ] BMI270 step detection drives player movement speed
- [ ] Multi-sensor: second Cardputer triggers "PLAYER 2 HAS ENTERED THE GAME"

---

## Repo layout

```
doomagotchi/
├── ADR/                        # architecture decision records
│   ├── 0001-vendor-doomgeneric.md
│   ├── 0002-tab5-renders-cardputer-senses.md
│   └── 0003-lora-event-protocol.md
├── proto/
│   └── dgm_event.h             # shared header, single source of truth
├── doom-node/                  # Tab5, ESP-IDF
│   ├── main/
│   └── third_party/doomgeneric/  # SUBMODULE — DO NOT EDIT
└── pwn-node/                   # Cardputer Adv, ESP-IDF
    └── main/
```

Write ADR 0001 and 0002 before writing code. They exist so that six weeks from now, when
something tempts us to patch DOOM's game logic, the reasoning for why we don't is on record.

---

## Legal and scope

This is a passive receive-only monitoring project. The sensor listens on channels it is
already able to hear. **No deauthentication frames, no injection, no active attacks** —
those are out of scope, and adding them would put the repo in a category we're not
publishing into. Handshake capture from ambient traffic only.
