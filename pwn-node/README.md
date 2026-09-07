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
- [ ] **ST7789 240×135 render** — the mood face + status bar. Needs the Cardputer
      in hand: it's untestable pixel code, and the "real DOOM status bar" wants
      the Freedoom `STF*` / `STTNUM*` lumps (WAD picture-format decode). Deferred
      until the board is connected.

Everything above `ST7789 render` builds clean and runs headless. Not flashed on
hardware yet — needs the Cardputer connected.

## Cardputer Adv pins (for later steps)

- Display ST7789: SCLK 36, MOSI 35, DC 34, CS 37, RST 33, BL 38; `SPI3_HOST`;
  `invert_color`, gap x=52 y=40.
- microSD (SPI, separate bus): SCK 40, MISO 39, MOSI 14, CS 12. `gpio_reset_pin`
  + drive CS high *before* mount (the sdspi driver's own CS setup must be a
  clean second touch).
- Keyboard: TCA8418 I2C, SDA 8 / SCL 9 / INT 11 (cheats later, not Phase 2).
