#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Passive 2.4 GHz sensor for the DOOMAGOTCHI pwn node. Puts the S3 radio in
// promiscuous mode, hops channels on core 0, parses beacons/probe-responses
// for the encryption tier and EAPOL-Key frames for handshake / PMKID captures.
//
// Emits `struct dgm_event` (proto/dgm_event.h) onto `out_q` — one AP_SEEN per
// newly-seen BSSID (5-min dedup), HANDSHAKE on a paired M1+M2, PMKID on an M1
// carrying a PMKID KDE. Nothing is ever transmitted.

// Start capture. `out_q` must be a QueueHandle_t of sizeof(struct dgm_event).
void wifi_sniff_start(QueueHandle_t out_q);

// Coarse counters for the status bar / debug.
typedef struct {
    uint32_t frames;      // total promiscuous frames seen
    uint32_t beacons;     // mgmt beacon + probe-resp
    uint32_t eapol;       // EAPOL-Key frames
    uint32_t aps;         // distinct BSSIDs currently tracked
    uint32_t handshakes;  // paired 4-way captures this session
    uint32_t pmkids;      // PMKID captures this session
    uint8_t  channel;     // current hop channel
    int8_t   last_rssi;
} wifi_sniff_stats_t;

void wifi_sniff_get_stats(wifi_sniff_stats_t *out);
