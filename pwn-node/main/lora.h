// lora.h — SX1262 (M5 LoRa-1262 CAP) transmit path for the pwn node.
//
// Phase 3, sensor side: the 6-byte `struct dgm_event` the sensor currently
// serial-prints (see main.c) goes out over sub-GHz LoRa instead. TX only — the
// sensor never listens on this radio (ADR 0002 "receive-only" is about 2.4 GHz;
// this is a different band and a different radio, and we only ever key it to
// announce our own events).
//
// The SX1262 shares the microSD SPI bus (SCK 40 / MOSI 14 / MISO 39, SPI2_HOST).
// pcap_wad.c owns the bus init and the SD device; we add a second device on the
// same host for NSS (GPIO5). The ESP-IDF SPI master driver serialises
// transactions per bus, so the pcap writer task and lora_send_event() can call
// in from different cores without extra locking.
//
// Control lines on the CAP header: NSS 5, DIO1 4 (TxDone IRQ, unused - we poll),
// RST 3, BUSY 6. (GPS UART on 15/13 is ignored here.)
//
// ---------------------------------------------------------------------------
//  ON-AIR PARAMETERS - the wire contract. The DOOM-node RX must match exactly.
//  Region: US 902-928 MHz ISM (confirmed 915 SKU).
//
//    Frequency ......... 915.000 MHz
//    Modulation ........ LoRa, SF9, BW 125 kHz, CR 4/5
//    Header ............ explicit, payload CRC on, IQ standard
//    Preamble ......... 12 symbols
//    Sync word ........ 0x1424 (SX126x "private network")
//    Payload .......... sizeof(struct dgm_event) == 6 bytes, fixed
//    TX power ......... +14 dBm (bench-friendly; bump LORA_TX_DBM for range)
//
//  ~6-byte packet airtime is ~40 ms. Events are seconds apart, so duty cycle
//  is a non-issue on the US ISM band.
// ---------------------------------------------------------------------------

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dgm_event.h"

typedef struct {
    bool     present;          // radio answered on SPI and configured OK
    uint32_t sent;             // events that reached TxDone
    uint32_t tx_timeouts;      // SetTx keyed but no TxDone before the deadline
    uint32_t errors;           // SPI / device-error failures
    uint32_t last_airtime_ms;  // wall-clock SetTx -> TxDone of the last packet
} lora_stats_t;

// Bring the SX1262 up: hardware reset, standby, TCXO + image cal, LoRa config,
// and (antenna is attached) one probe transmit to confirm the PA keys and
// TxDone fires. Returns false and leaves the sensor fully functional if the
// module doesn't respond - same contract as pcap_wad_start() / display_start().
bool lora_init(void);

// Transmit one event. Blocking, bounded (~500 ms worst case). Safe to call
// from the main loop while the pcap writer task is hitting the SD on the same
// SPI bus. Returns false on SPI error or TX timeout; a dropped event is just a
// monster that doesn't spawn (ADR 0003), so callers can ignore the result.
bool lora_send_event(const struct dgm_event *ev);

void lora_get_stats(lora_stats_t *out);
