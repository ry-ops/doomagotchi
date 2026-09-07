# ADR 0003 — The pwn↔DOOM link carries events, not game state

- **Status:** Accepted
- **Date:** 2026-09-06
- **Deciders:** project owner

## Context

The pwn node (sensor) and the DOOM node (renderer) are separate boards
(ADR 0002) joined only by a sub-GHz SX1262 LoRa link. LoRa is low bandwidth
(single-digit kbps at usable range) and lossy. We have to decide what actually
crosses it.

Two models:

1. **State sync** — the sensor is authoritative for "the world," streams
   positions / health / monster lists, the renderer mirrors them. This is what
   a modern netgame does.
2. **Event feed** — each node simulates locally; the link carries only discrete
   "something happened" notifications, and each side reacts.

DOOM's own 1993 netcode is model 2 in spirit: a few bytes of `ticcmd` per
player per tic over a serial/IPX link, each node running the same deterministic
sim. It never shipped the world over the wire.

## Decision

**Event feed. No state sync.** The wire format is `struct dgm_event`
(`proto/dgm_event.h`), **6 bytes, packed**, and is the single source of truth
shared by both projects verbatim:

```c
struct dgm_event {
    uint8_t version;      // DGM_PROTO_VERSION
    uint8_t unit_id;      // which sensor (multi-sensor, Phase 4)
    uint8_t event_type;   // AP_SEEN | HANDSHAKE | PMKID | AP_LOST | SESSION_END
    uint8_t enemy_class;  // Zombieman..Baron, from the AP's encryption suite
    uint8_t rssi_bucket;  // 0..7 -> spawn distance
    uint8_t bssid_hash;   // stable 8-bit id, for dedupe only
};
```

- The **sensor** decides an AP's `enemy_class` (open→Zombieman, WEP→Shotgun
  Guy, WPA2-PSK→Imp, WPA3-SAE→Cacodemon, hidden→Spectre, 802.1X→Baron) and
  `rssi_bucket`. It never knows or cares where the monster ends up in the map.
- The **DOOM node** owns placement: on `AP_SEEN` it spawns that `enemy_class`
  via `P_SpawnMobj` at a distance derived from `rssi_bucket`, dedup'd by
  `bssid_hash`. On `HANDSHAKE` / `PMKID` it kills the matching monster (which
  weapon the autoplayer used is the only difference). `SESSION_END` triggers
  the intermission screen.
- `bssid_hash` is 8-bit on purpose: it only has to disambiguate the handful of
  monsters alive at once, not identify an AP globally. Collisions cost one
  wrong despawn, nothing more.

## Consequences

**Positive**

- A dropped packet is a monster that didn't spawn or didn't die on cue — never
  a desync, never a crash. No acks, no retransmit, no sequence numbers.
- The two codebases share exactly one header and no runtime coupling. Either
  runs standalone (the pwn node serial-prints the events it would send; the
  DOOM node autoplays with no sensor).
- Fits LoRa's envelope with enormous margin — 6 bytes every few seconds.

**Negative / accepted costs**

- The DOOM node can't show anything the sensor knows beyond these 5 fields
  (no SSID text, no channel, no vendor). Richer telemetry, if ever wanted, is
  a separate optional channel, not this one.
- "Which real AP is which on-screen monster" is only ever approximate
  (8-bit hash). Acceptable — this is a screensaver, not a survey tool.

## Notes

Phase 2 implements the sensor side (`pwn-node/main/wifi_sniff.c` emits these
onto a queue; `main.c` prints them). Phase 3 replaces the print with an
SX1262 TX and adds the DOOM-node RX + spawn/kill glue. The format does not
change between those phases.
