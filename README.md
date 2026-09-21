# DOOMAGOTCHI

A passive, self-playing DOOM that renders the live 2.4 GHz RF environment as demons.

Nobody plays it. It plays itself, like a pwnagotchi. You carry a sensor around town;
nearby access points spawn as monsters; captured handshakes are kills; the
end-of-session tally is a DOOM intermission screen.

Two devices, joined over sub-GHz LoRa, running **actual DOOM** — not a clone, not a
"DOOM-like." It runs on the [Freedoom](https://freedoom.github.io/) Phase 1 IWAD, a
free, MIT/GPL-compatible asset set with the same map layout and engine compatibility
as the original — **not** id Software's commercial `DOOM.WAD`, which this repo never
ships or references. See [`CLAUDE.md`](./CLAUDE.md) for the full project brief and the
non-negotiable constraints this repo is built under.

## How it works

- **`pwn-node`** (M5Stack Cardputer Adv, ESP32-S3) walks around and passively listens.
  It puts its WiFi radio in promiscuous mode, channel-hops, and classifies every AP it
  hears by encryption suite. A captured four-way handshake or PMKID is a kill. It never
  transmits on 2.4 GHz, never deauths, never connects — receive-only.
- **`doom-node`** (M5Stack Tab5, ESP32-P4) runs unmodified
  [`doomgeneric`](https://github.com/ozkl/doomgeneric) on real Freedoom assets. It has
  no radio of its own; it just renders. RF observations arrive as spawn/kill events and
  drive the game the same way a joystick would — nothing about DOOM's own logic changes.
- The two talk over an **SX1262 LoRa** link, sub-GHz, sharing no band with the capture
  radio. The wire format is a 6-byte `struct dgm_event` ([`proto/dgm_event.h`](./proto/dgm_event.h),
  the single source of truth both firmwares include verbatim). No game state crosses the
  link — only discrete events, in the spirit of 1993 `ticcmd` netcode. A dropped packet
  is just a monster that doesn't spawn.

| RF observation       | Enemy         |
|-----------------------|---------------|
| Open network           | Zombieman     |
| WEP                     | Shotgun Guy   |
| WPA2-PSK                | Imp           |
| WPA3-SAE                | Cacodemon     |
| Hidden SSID              | Spectre       |
| 802.1X / Enterprise      | Baron of Hell |

## Repo layout

```
doomagotchi/
├── ADR/                           # architecture decision records
├── proto/dgm_event.h              # shared wire protocol, single source of truth
├── doom-node/                     # Tab5 firmware, ESP-IDF
│   └── third_party/doomgeneric/   # git submodule, vendored read-only
├── pwn-node/                      # Cardputer Adv firmware, ESP-IDF
└── tools/                         # host-side helpers (e.g. WAD transfer over serial)
```

Each firmware project has its own README with build/flash instructions and a detailed
implementation log: [`doom-node/README.md`](./doom-node/README.md),
[`pwn-node/README.md`](./pwn-node/README.md).

## Why it's built this way

Three ADRs record the reasoning, written before code so it doesn't get relitigated
later:

- [ADR 0001](./ADR/0001-vendor-doomgeneric.md) — vendor `doomgeneric`, never write a
  renderer.
- [ADR 0002](./ADR/0002-tab5-renders-cardputer-senses.md) — why the Tab5 runs DOOM and
  the Cardputer does the RF capture (forced by PSRAM and radio placement, not taste).
- [ADR 0003](./ADR/0003-lora-event-protocol.md) — why the link carries events, not
  synced game state.

## Status

| Phase | What | Status |
|-------|------|--------|
| 0 | DOOM boots on the Tab5, playable by hand | ✅ done |
| 1 | Autoplayer — hunts, routes, and kills unattended | ✅ done |
| 2 | Sensor standalone — capture, classify, mood, status LCD | ✅ done |
| 3 | LoRa seam | 🚧 TX verified on hardware (pwn-node); DOOM-node RX + spawn glue not started |
| 4 | Polish — intermission stats, GPS level naming, step-driven movement, multiplayer | ⬜ not started |

Hardware notes and verification dates live in each node's README; treat those as the
up-to-date log.

## Assets and legal

Uses the **[Freedoom](https://freedoom.github.io/) Phase 1 IWAD** — never a commercial
`DOOM.WAD`. The sensor is **passive receive-only**: no deauthentication, no injection,
no active attacks. It captures handshakes from ambient traffic it can already hear.
This scope is intentional and non-negotiable; see `CLAUDE.md`.
