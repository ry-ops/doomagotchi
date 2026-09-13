# pwn-node — DOOMAGOTCHI sensor (M5Stack Cardputer Adv / ESP32-S3)

Passive 2.4 GHz sensor. **Receive-only** — no deauth, no injection, no active
scan, never connects (ADR 0002, CLAUDE.md "Legal and scope").

## Build & flash

```sh
. $IDF_PATH/export.sh
cd pwn-node
idf.py set-target esp32s3
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Builds and runs standalone — no Tab5 needed.

## Phase 2 progress

- [x] **project skeleton** (esp32s3)
- [x] **promiscuous capture + channel hop** — `wifi_sniff.c`: `WIFI_MODE_STA`
      (never connects), promiscuous filter MGMT+DATA, hop 1/6/11 then the rest
      on a 300 ms esp_timer.
- [x] **beacon / probe-resp parse → encryption tier** — SSID (tag 0, empty/all-zero
      = hidden → Spectre), RSN IE (tag 48), WPA IE (vendor 00:50:F2 t1); AKM
      suite list → 802.1X→Baron / SAE→Cacodemon / PSK→Imp; privacy-only→Shotgun
      Guy; open→Zombieman. 96-entry BSSID table, 5-min dedup, emits `AP_SEEN`.
- [x] **EAPOL-Key handshake + PMKID** — LLC/SNAP `…88 8E`, Key Information bits:
      M1 (ACK, !MIC) carries the PMKID KDE (`DD .. 00 0F AC 04 <16>`) → `PMKID`;
      M1→M2 within 5 s → `HANDSHAKE`. Both are opportunistic (real client
      (re)associating), never on a timer.
- [x] **emit `dgm_event`** onto a queue; `main.c` prints each one (the packet it
      *would* LoRa in Phase 3) + a 5 s stats line.
- [x] **pcap writer to SD** — `pcap_wad.c`: mounts microSD (SPI), appends one
      beacon per BSSID + every EAPOL frame to a rotating `CAPnnnn.WAD` (plain
      little-endian pcap, linktype 105). Writes run on a core-1 task fed by a
      24-slot pool so the RX callback never blocks; drops instead of stalling.
      No card → module no-ops, sensor runs on.
- [x] **mood state machine** — `mood.c`: the *inverted* pwnagotchi. Busy airspace
      → manic/rampage; dead air → bored/mean. 60 s AP window + 120 s capture
      window drive `MOOD_BORED/RESTLESS/HUNTING/MANIC/RAMPAGE`, an intensity
      0–255 (face animation / blink rate later), and a rotating DOOM-flavored
      quip. Pure logic; feed it every event + a 1 Hz tick. For now it prints on
      the 5 s stats line.
- [x] **ST7789 240×135 render** — `display.c`: `esp_lcd` ST7789 on SPI3 (SD is on
      SPI2), 1-bit black-on-white framebuffer blitted ~1 Hz, drawn in the
      **pwnagotchi idiom**. Big centred kaomoji face, one per mood, blinking:
      `(-_-)` bored · `(~_~)` restless · `(o_o)` hunting · `(@_@)` manic ·
      `(x_x)` rampage. Top bar `CH n  APS n │ UP h:mm:ss`, the mood quip as the
      "voice" line, a corner readout (`HS / PMK / MOOD iNNN`), bottom bar
      `PWND n (aps)  [LAST-ENEMY]`. Mono palette (0x0000/0xFFFF) is
      byte-order-invariant so the RGB565 endian question doesn't apply. Embedded
      5×7 glcdfont. No panel → `display_start()` returns false, sensor headless.
      Confirmed on glass 2026-09-06.

**Phase 2 verified on hardware** (ESP32-S3, 2026-09-06): promiscuous capture +
classify solid (~300 beacons/10 s, 30+ APs tiered), channel hop + RSSI live,
mood machine sane (idles ~i60), ST7789 face + status bar render with correct
rotation, SD mounts and `CAPnnnn.WAD` grows with zero drops. Only the
EAPOL/PMKID paths are unexercised — they need a real client (re)associating and
this is passive-only, so nothing forces one.

## Phase 3 progress — the LoRa seam (sensor side)

- [x] **SX1262 TX driver** — `lora.c`: hand-rolled SX126x command set, TX only.
      915.000 MHz, LoRa SF9 / BW 125 kHz / CR 4/5, explicit header + CRC,
      preamble 12, sync word `0x1424`, +14 dBm, 6-byte `struct dgm_event`
      payload (~140 ms airtime — matches the Semtech time-on-air formula for
      SF9/BW125 almost exactly; the original "~40 ms" estimate here was just
      wrong). Shares the microSD SPI bus (`SPI2_HOST`,
      SCK 40 / MOSI 14 / MISO 39) as a second `spi_device` on NSS 5; the SPI
      master driver serialises against the pcap writer task, no extra lock.
      RST 3, BUSY 6 polled; DIO1 4 left alone (we poll `GetIrqStatus`). TCXO on
      DIO3 @ 1.8 V + full recalibration; init logs `GetDeviceErrors` so a wrong
      TCXO assumption is visible on the first flash (`LORA_USE_TCXO 0` to flip).
      `lora_init()` returns false and the sensor runs on if the module is
      absent — same contract as SD / LCD.
- [x] **wired into `main.c`** — the `dgm_event` we used to only serial-print is
      now also handed to `lora_send_event()`; the 5 s stats line reports
      `sent / timeouts / errs / last airtime`. A drop is just a monster that
      doesn't spawn (ADR 0003), so the send result is advisory.
- [x] **verify on hardware** — confirmed live on the bench (2026-09-13): probe
      TX keys clean at boot, then sustained sends over a normal run —
      `sent=56 timeouts=0 errs=0 last=151ms` on the 5 s stats line, zero SPI
      errors, zero chip-side TX timeouts. Cardputer sensor side of Phase 3 is
      done. Not yet checked: an actual receiver decoding the frames (needs the
      DOOM-node RX below).
- [ ] **DOOM-node RX + spawn/kill glue** — the other half of Phase 3, entirely
      on the Tab5 side. Nothing here to do until that's picked up.

### Cardputer ADV gotcha — SD peripheral rails

The ADV isolates its SD slot (and other rails) behind enable lines the ROM
leaves low. `pcap_wad_start()` drives **GPIO 3/4/5/6/13/15 high** before SPI —
without GPIO5 in particular the card never answers CMD8 (`send_if_cond` →
`0x108`). Matches M5's own firmware bring-up.

## Cardputer Adv pins (for later steps)

- Display ST7789: SCLK 36, MOSI 35, DC 34, CS 37, RST 33, BL 38; `SPI3_HOST`;
  `invert_color`, gap x=52 y=40.
- microSD (SPI, separate bus): SCK 40, MISO 39, MOSI 14, CS 12. `gpio_reset_pin`
  + drive CS high *before* mount (the sdspi driver's own CS setup must be a
  clean second touch).
- Keyboard: TCA8418 I2C, SDA 8 / SCL 9 / INT 11 (cheats later, not Phase 2).
