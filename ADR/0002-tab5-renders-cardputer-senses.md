# ADR 0002 — The Tab5 renders DOOM; the Cardputer senses RF

- **Status:** Accepted
- **Date:** 2026-09-06
- **Deciders:** project owner

## Context

DOOMAGOTCHI needs two capabilities at once:

1. **Run unmodified DOOM.** DOOM's zone allocator claims one large contiguous heap
   (historically 6 MB; we will size ours from real numbers in Phase 0). Sprites,
   composited textures, level data and the drawseg/vissprite scratch all live in it.
   A device that cannot give DOOM multiple contiguous megabytes cannot run it —
   see ADR 0001, we are not shrinking DOOM to fit.
2. **See raw 802.11 frames.** Handshake / PMKID capture needs the radio in
   promiscuous mode, delivering management and EAPOL frames to a sniffer callback.

No single board in the parts bin does both well. The split below is forced by that,
not chosen for elegance.

### M5Stack Tab5 — ESP32-P4

- RISC-V dual-core @ 360 MHz, 16 MB flash, **32 MB PSRAM**
- 5" 1280×720 IPS over MIPI-DSI
- Wireless is a **separate ESP32-C6** on an SDIO bus (`esp_hosted`)

The 32 MB of PSRAM is the whole reason this board runs DOOM — it is the only board
here with contiguous megabytes to spare. But the P4 has **no radio of its own**.
802.11 frames terminate on the C6 and cross to the P4 as an `esp_hosted` netif;
the P4 never sees a raw frame. Promiscuous capture would mean writing and
maintaining custom hosted-firmware for the C6 — a large, open-ended detour. We route
around it: the Tab5 does no RF capture.

### M5Stack Cardputer Adv — ESP32-S3FN8

- Xtensa dual-core @ 240 MHz, 8 MB flash, **no PSRAM** (≈512 KB internal SRAM)
- 240×135 ST7789V2 LCD, 56-key keyboard, BMI270 IMU, microSD, SX1262 LoRa cap

`esp_wifi_set_promiscuous()` is a first-class ESP-IDF call on the S3 — the radio is
on-die, the sniffer callback just works. Pin the radio + channel-hop timer to core 0,
everything else to core 1. But 512 KB of internal SRAM **cannot hold DOOM**, and per
ADR 0001 we do not make a smaller DOOM. So the Cardputer runs no engine.

## Decision

Two devices, two ESP-IDF projects in one repo, joined only by a 6-byte LoRa event
(ADR 0003):

| | **DOOM node** | **pwn node** |
|---|---|---|
| Board | M5Stack Tab5 (ESP32-P4) | M5Stack Cardputer Adv (ESP32-S3) |
| Dir | `doom-node/` | `pwn-node/` |
| Runs | unmodified `doomgeneric` + autoplayer | promiscuous sniffer + `.WAD` pcap writer |
| Screen | 1280×720, full first-person DOOM | 240×135, DOOM status bar only (face, kills, ammo, health, channel) |
| RF | none | 2.4 GHz capture (RX only), sub-GHz LoRa TX |
| Talks via | receives `dgm_event` | sends `dgm_event` |

- The two projects **build and run independently**. `pwn-node` is useful on its own
  (a sniffer with a DOOM face); `doom-node` is useful on its own (Freedoom on a
  handheld). The LoRa link is additive, not a dependency (Phase 3).
- No shared game state crosses the link. Each node simulates locally. LoRa carries
  events only.
- The 240×135 panel showing *only* the real DOOM status bar is a feature: it is
  almost exactly the aspect ratio of the original status bar strip, and the reactive
  face is the pwnagotchi mood display for free.

## Consequences

**Positive**

- Each board does only what its silicon is good at. No fighting PSRAM limits on the
  S3, no writing C6 hosted-firmware for the P4.
- Clean failure isolation: a sniffer crash cannot corrupt the running game, and vice
  versa — they share nothing but a lossy 6-byte radio message.
- The protocol is so small (ADR 0003) that "sync" bugs are nearly impossible; a
  dropped event is just a monster that didn't spawn.

**Negative / accepted costs**

- Two build targets, two toolchains (`riscv32-esp-elf` and `xtensa-esp32s3-elf`),
  two flash/monitor workflows. CI runs both.
- Physical: two devices to carry and charge. The LoRa range (km) means they need not
  be co-located, which turns this cost into part of the concept.
- The autoplayer on the DOOM node cannot get ground-truth from the sniffer beyond
  what fits in `dgm_event`. Accepted — richer telemetry is a later, optional channel.

## Revisit if

- A single board appears with both on-die 2.4 GHz promiscuous capture **and**
  contiguous multi-MB RAM for the zone. Then the split is no longer forced and this
  ADR should be reconsidered.
- `esp_hosted` gains a supported promiscuous/monitor passthrough on the P4↔C6 bus.
  That removes the "no RF on the Tab5" constraint but not the "no DOOM on the
  Cardputer" one, so the split likely still stands.
