// dgm_event.h — DOOMAGOTCHI LoRa event protocol
//
// Single source of truth for the wire format between the pwn node (sensor,
// ESP32-S3) and the DOOM node (renderer, ESP32-P4). Both projects include this
// header verbatim; nothing else crosses the link.
//
// Design: we do NOT sync game state. Each node simulates locally. LoRa carries
// discrete events only — a few bytes per observation, in the spirit of 1993
// serial ticcmd netcode. A dropped packet is just a monster that didn't spawn.
//
// See ADR/0003-lora-event-protocol.md for rationale.

#ifndef DGM_EVENT_H
#define DGM_EVENT_H

#include <stdint.h>

#define DGM_PROTO_VERSION 1

// event_type
enum {
    DGM_EV_AP_SEEN     = 0, // a (new-ish) access point is in range -> spawn
    DGM_EV_HANDSHAKE   = 1, // full 4-way EAPOL capture           -> kill (chainsaw)
    DGM_EV_PMKID       = 2, // PMKID capture, the fast kill        -> kill (shotgun)
    DGM_EV_AP_LOST     = 3, // AP no longer heard                  -> despawn / flee
    DGM_EV_SESSION_END = 4, // sensor ended a session             -> intermission
};

// enemy_class — derived from the AP's advertised encryption suite.
// Initial mapping (tune later); mirrors the table in CLAUDE.md.
enum {
    DGM_ENEMY_ZOMBIEMAN = 0, // open network
    DGM_ENEMY_SHOTGUN_GUY = 1, // WEP
    DGM_ENEMY_IMP = 2, // WPA2-PSK
    DGM_ENEMY_CACODEMON = 3, // WPA3-SAE
    DGM_ENEMY_SPECTRE = 4, // hidden SSID
    DGM_ENEMY_BARON = 5, // 802.1X / Enterprise
};

// rssi_bucket: 0 = weakest/farthest .. 7 = strongest/closest. The DOOM node
// maps this to spawn distance from the player actor.
#define DGM_RSSI_BUCKETS 8

// 6 bytes on the wire. Keep it packed and keep it 6.
struct dgm_event {
    uint8_t version;     // DGM_PROTO_VERSION
    uint8_t unit_id;     // which sensor (supports multi-sensor, Phase 4)
    uint8_t event_type;  // DGM_EV_*
    uint8_t enemy_class; // DGM_ENEMY_*
    uint8_t rssi_bucket; // 0..7
    uint8_t bssid_hash;  // stable 8-bit id of the BSSID, for dedupe only
} __attribute__((packed));

_Static_assert(sizeof(struct dgm_event) == 6, "dgm_event must be 6 bytes on the wire");

#endif // DGM_EVENT_H
