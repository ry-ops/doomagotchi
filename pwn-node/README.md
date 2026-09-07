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
- [ ] pcap writer to SD, files named `*.WAD`
- [ ] ST7789 240×135: real DOOM status bar + reactive mood face
- [ ] mood state machine (inverted pwnagotchi)

Not built/flashed on hardware yet — needs the Cardputer connected.

## Cardputer Adv pins (for later steps)

- Display ST7789: SCLK 36, MOSI 35, DC 34, CS 37, RST 33, BL 38; `SPI3_HOST`;
  `invert_color`, gap x=52 y=40.
- microSD (SPI, separate bus): SCK 40, MISO 39, MOSI 14, CS 12. `gpio_reset_pin`
  + drive CS high *before* mount (the sdspi driver's own CS setup must be a
  clean second touch).
- Keyboard: TCA8418 I2C, SDA 8 / SCL 9 / INT 11 (cheats later, not Phase 2).
