#pragma once
#include <stdbool.h>
#include <stdint.h>

// Capture-file writer for the DOOMAGOTCHI pwn node.
//
// Mounts the Cardputer's microSD (SPI) and appends the raw 802.11 frames that
// matter for offline analysis -- one beacon per BSSID (for the ESSID) plus
// every EAPOL-Key frame -- to a rotating pcap file. Files are named CAPnnnn.WAD:
// it's a plain little-endian pcap (linktype 105, IEEE 802.11), the extension is
// just the house style. hcxpcapngtool / wpaclean / aircrack-ng read them as-is.
//
// SD writes happen on a dedicated core-1 task fed by a lock-free-ish queue, so
// the promiscuous RX callback never blocks on SPI. If the card is missing or
// the mount fails the whole module no-ops and the sensor keeps running.

// Mount SD and start the writer task. Safe to call once. Returns false if SD
// is unavailable (the offer() calls below then just drop frames).
bool pcap_wad_start(void);

// Hand a frame to the writer. `frame`/`len` is the promiscuous payload as given
// by esp_wifi (FCS included). Copies into a pool slot and returns immediately;
// drops the frame if the pool/queue is full or SD isn't mounted. Call from the
// RX callback.
void pcap_wad_offer(const uint8_t *frame, uint16_t len, int64_t ts_us);

typedef struct {
    bool     mounted;
    uint32_t written;    // frames appended
    uint32_t dropped;    // frames lost to a full pool/queue
    uint32_t bytes;      // bytes in the current file
    uint16_t file_index; // n in CAPnnnn.WAD
} pcap_wad_stats_t;

void pcap_wad_get_stats(pcap_wad_stats_t *out);
