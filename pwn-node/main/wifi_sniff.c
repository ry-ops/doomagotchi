#include "wifi_sniff.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "dgm_event.h"

static const char *TAG = "sniff";

// ---------------------------------------------------------------------------
// 802.11 bits
// ---------------------------------------------------------------------------
#define FC_TYPE(fc)     (((fc) >> 2) & 0x3)
#define FC_SUBTYPE(fc)  (((fc) >> 4) & 0xF)
#define FC_TODS(fc)     ((fc) & 0x0100)
#define FC_FROMDS(fc)   ((fc) & 0x0200)
#define TYPE_MGMT  0
#define TYPE_DATA  2
#define ST_PROBE_RESP  5
#define ST_BEACON      8
#define ST_QOS_DATA    8   // subtypes 8..11 are QoS variants

// RSN/WPA AKM suite selector low byte (OUI 00:0F:AC or 00:50:F2)
#define AKM_8021X   1
#define AKM_PSK     2
#define AKM_FT_8021X 3
#define AKM_FT_PSK  4
#define AKM_8021X_SHA256 5
#define AKM_PSK_SHA256   6
#define AKM_SAE     8
#define AKM_FT_SAE  9

static const uint8_t HOP_SEQ[] = { 1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13 };

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
static QueueHandle_t s_out_q;
static esp_timer_handle_t s_hop_timer;
static volatile uint8_t s_hop_idx;

static wifi_sniff_stats_t s_stats;

#define AP_TABLE_N   96
#define AP_EXPIRE_US (5LL * 60 * 1000 * 1000)
typedef struct {
    uint8_t bssid[6];
    uint8_t enemy_class;
    int64_t last_us;
} ap_ent_t;
static ap_ent_t s_aps[AP_TABLE_N];

#define PEND_N        16
#define PEND_WINDOW_US (5LL * 1000 * 1000)
typedef struct {
    uint8_t bssid[6];
    int64_t m1_us;
} pend_t;
static pend_t s_pend[PEND_N];

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static uint8_t rssi_bucket(int8_t rssi)
{
    int v = (rssi + 96) * (DGM_RSSI_BUCKETS - 1) / 56;   // -40dBm -> 7, -96 -> 0
    if (v < 0) v = 0;
    if (v > DGM_RSSI_BUCKETS - 1) v = DGM_RSSI_BUCKETS - 1;
    return (uint8_t)v;
}

static uint8_t bssid_hash8(const uint8_t b[6])
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) { h = (h ^ b[i]) * 16777619u; }
    return (uint8_t)(h ^ (h >> 8) ^ (h >> 16) ^ (h >> 24));
}

static void emit(uint8_t type, uint8_t enemy, uint8_t bucket, const uint8_t bssid[6])
{
    if (!s_out_q) return;
    struct dgm_event ev = {
        .version = DGM_PROTO_VERSION,
        .unit_id = 0,
        .event_type = type,
        .enemy_class = enemy,
        .rssi_bucket = bucket,
        .bssid_hash = bssid_hash8(bssid),
    };
    xQueueSend(s_out_q, &ev, 0);
}

// AKM suite list -> DGM_ENEMY_*. akm points at "AKM suite count" (LE16) then
// count * 4-byte selectors. `privacy` from the capability info.
static uint8_t classify(const uint8_t *rsn, size_t rsn_len, const uint8_t *wpa, size_t wpa_len,
                        bool privacy, bool hidden)
{
    if (hidden) return DGM_ENEMY_SPECTRE;

    const uint8_t *akm = NULL;
    size_t rem = 0;
    if (rsn && rsn_len >= 8) {
        // version(2) group(4) pairwise_count(2) pairwise(n*4) akm_count(2) akm(n*4)
        size_t off = 2 + 4;
        if (off + 2 <= rsn_len) {
            uint16_t pc = rsn[off] | (rsn[off + 1] << 8);
            off += 2 + (size_t)pc * 4;
            if (off + 2 <= rsn_len) { akm = rsn + off; rem = rsn_len - off; }
        }
    } else if (wpa && wpa_len >= 10) {
        // wpa vendor payload: OUI(3) type(1) version(2) group(4) pairwise_count(2) pairwise akm_count(2) akm
        size_t off = 4 + 2 + 4;
        if (off + 2 <= wpa_len) {
            uint16_t pc = wpa[off] | (wpa[off + 1] << 8);
            off += 2 + (size_t)pc * 4;
            if (off + 2 <= wpa_len) { akm = wpa + off; rem = wpa_len - off; }
        }
    }

    if (akm && rem >= 2) {
        uint16_t ac = akm[0] | (akm[1] << 8);
        const uint8_t *sel = akm + 2;
        rem -= 2;
        bool sae = false, ent = false, psk = false;
        for (uint16_t i = 0; i < ac && rem >= 4; i++, sel += 4, rem -= 4) {
            uint8_t t = sel[3];
            if (t == AKM_SAE || t == AKM_FT_SAE) sae = true;
            else if (t == AKM_8021X || t == AKM_FT_8021X || t == AKM_8021X_SHA256) ent = true;
            else if (t == AKM_PSK || t == AKM_FT_PSK || t == AKM_PSK_SHA256) psk = true;
        }
        if (ent) return DGM_ENEMY_BARON;      // 802.1X / Enterprise
        if (sae) return DGM_ENEMY_CACODEMON;  // WPA3-SAE
        if (psk) return DGM_ENEMY_IMP;        // WPA2-PSK
    }

    if (rsn || wpa) return DGM_ENEMY_IMP;     // encrypted, AKM unclear -> treat as WPA2
    if (privacy)    return DGM_ENEMY_SHOTGUN_GUY;  // WEP (privacy bit, no RSN/WPA)
    return DGM_ENEMY_ZOMBIEMAN;               // open
}

static ap_ent_t *ap_lookup_or_add(const uint8_t bssid[6], int64_t now, bool *is_new)
{
    ap_ent_t *free_slot = NULL, *oldest = &s_aps[0];
    for (int i = 0; i < AP_TABLE_N; i++) {
        ap_ent_t *e = &s_aps[i];
        if (e->last_us == 0) { if (!free_slot) free_slot = e; continue; }
        if (memcmp(e->bssid, bssid, 6) == 0) {
            *is_new = (now - e->last_us) > AP_EXPIRE_US;
            e->last_us = now;
            return e;
        }
        if (e->last_us < oldest->last_us) oldest = e;
    }
    ap_ent_t *e = free_slot ? free_slot : oldest;
    memcpy(e->bssid, bssid, 6);
    e->last_us = now;
    *is_new = true;
    return e;
}

// ---------------------------------------------------------------------------
// EAPOL
// ---------------------------------------------------------------------------
static void handle_eapol(const uint8_t *e, size_t elen, const uint8_t bssid[6], int64_t now)
{
    // e -> EAPOL header: version, type, length(BE16)
    if (elen < 4 || e[1] != 3) return;   // type 3 = EAPOL-Key
    const uint8_t *k = e + 4;
    size_t klen = elen - 4;
    if (klen < 1 + 2 + 2 + 8 + 32 + 16 + 8 + 8 + 16 + 2) return;

    uint16_t info = (k[1] << 8) | k[2];   // key information, BE
    bool pairwise = info & 0x0008;
    bool install  = info & 0x0040;
    bool ack      = info & 0x0080;
    bool mic      = info & 0x0100;
    bool secure   = info & 0x0200;
    if (!pairwise) return;

    size_t kdl_off = 1 + 2 + 2 + 8 + 32 + 16 + 8 + 8 + 16;
    uint16_t kdl = (k[kdl_off] << 8) | k[kdl_off + 1];
    const uint8_t *kd = k + kdl_off + 2;
    if ((size_t)(kdl_off + 2 + kdl) > klen) kdl = 0;

    if (ack && !mic && !install) {
        // ----- M1 (AP -> STA) -----
        s_stats.eapol++;

        // PMKID KDE in key data: DD <len> 00 0F AC 04 <16>
        for (size_t i = 0; i + 2 <= kdl; ) {
            uint8_t t = kd[i], l = kd[i + 1];
            if (t != 0xDD || i + 2 + l > kdl) { i += 2 + (t == 0xDD ? l : 0); if (l == 0) break; continue; }
            if (l >= 20 && kd[i + 2] == 0x00 && kd[i + 3] == 0x0F && kd[i + 4] == 0xAC && kd[i + 5] == 0x04) {
                bool nonzero = false;
                for (int j = 0; j < 16; j++) if (kd[i + 6 + j]) { nonzero = true; break; }
                if (nonzero) {
                    bool is_new; ap_ent_t *ap = ap_lookup_or_add(bssid, now, &is_new);
                    s_stats.pmkids++;
                    emit(DGM_EV_PMKID, ap->enemy_class, 4, bssid);
                    ESP_LOGI(TAG, "PMKID  bssid=%02x:%02x:..:%02x class=%u",
                             bssid[0], bssid[1], bssid[5], ap->enemy_class);
                }
            }
            i += 2 + l;
        }

        // remember M1 time for pairing
        for (int i = 0; i < PEND_N; i++) {
            if (s_pend[i].m1_us == 0 || memcmp(s_pend[i].bssid, bssid, 6) == 0 ||
                (now - s_pend[i].m1_us) > PEND_WINDOW_US) {
                memcpy(s_pend[i].bssid, bssid, 6);
                s_pend[i].m1_us = now;
                break;
            }
        }
    } else if (mic && !ack && !secure && kdl > 0) {
        // ----- M2 (STA -> AP), carries the MIC = the crackable part -----
        s_stats.eapol++;
        for (int i = 0; i < PEND_N; i++) {
            if (s_pend[i].m1_us && memcmp(s_pend[i].bssid, bssid, 6) == 0 &&
                (now - s_pend[i].m1_us) <= PEND_WINDOW_US) {
                s_pend[i].m1_us = 0;
                bool is_new; ap_ent_t *ap = ap_lookup_or_add(bssid, now, &is_new);
                s_stats.handshakes++;
                emit(DGM_EV_HANDSHAKE, ap->enemy_class, 5, bssid);
                ESP_LOGI(TAG, "4-WAY  bssid=%02x:%02x:..:%02x class=%u",
                         bssid[0], bssid[1], bssid[5], ap->enemy_class);
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// promiscuous RX callback (runs on core 0, in the Wi-Fi task)
// ---------------------------------------------------------------------------
static void sniff_cb(void *buf, wifi_promiscuous_pkt_type_t ptype)
{
    if (ptype != WIFI_PKT_MGMT && ptype != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *p = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;
    if (len < 24) return;

    s_stats.frames++;
    s_stats.last_rssi = pkt->rx_ctrl.rssi;

    uint16_t fc = p[0] | (p[1] << 8);
    uint8_t type = FC_TYPE(fc), st = FC_SUBTYPE(fc);
    const uint8_t *bssid = p + 16;   // addr3
    int64_t now = esp_timer_get_time();

    if (type == TYPE_MGMT && (st == ST_BEACON || st == ST_PROBE_RESP)) {
        if (len < 36) return;
        s_stats.beacons++;
        uint16_t cap = p[34] | (p[35] << 8);
        bool privacy = cap & 0x10;
        bool hidden = false;
        const uint8_t *rsn = NULL, *wpa = NULL;
        size_t rsn_len = 0, wpa_len = 0;

        const uint8_t *q = p + 36;
        const uint8_t *end = p + len - 4;   // drop FCS
        while (q + 2 <= end) {
            uint8_t tag = q[0], tl = q[1];
            const uint8_t *td = q + 2;
            if (td + tl > end) break;
            if (tag == 0) {                       // SSID
                hidden = (tl == 0);
                if (!hidden) {
                    hidden = true;
                    for (int i = 0; i < tl; i++) if (td[i]) { hidden = false; break; }
                }
            } else if (tag == 48) {               // RSN
                rsn = td; rsn_len = tl;
            } else if (tag == 221 && tl >= 4 &&    // vendor: WPA (00:50:F2 type 1)
                       td[0] == 0x00 && td[1] == 0x50 && td[2] == 0xF2 && td[3] == 0x01) {
                wpa = td + 4; wpa_len = tl - 4;
            }
            q += 2 + tl;
        }

        bool is_new;
        ap_ent_t *ap = ap_lookup_or_add(bssid, now, &is_new);
        uint8_t cls = classify(rsn, rsn_len, wpa, wpa_len, privacy, hidden);
        ap->enemy_class = cls;
        if (is_new) {
            emit(DGM_EV_AP_SEEN, cls, rssi_bucket(pkt->rx_ctrl.rssi), bssid);
        }
        return;
    }

    if (type == TYPE_DATA) {
        int hlen = 24;
        if (FC_TODS(fc) && FC_FROMDS(fc)) hlen += 6;      // addr4
        if (st >= ST_QOS_DATA && st <= 11) hlen += 2;     // QoS control
        // LLC/SNAP for EAPOL: AA AA 03 00 00 00 88 8E
        if (len < hlen + 8 + 4) return;
        const uint8_t *l = p + hlen;
        if (l[0] == 0xAA && l[1] == 0xAA && l[2] == 0x03 &&
            l[3] == 0x00 && l[4] == 0x00 && l[5] == 0x00 &&
            l[6] == 0x88 && l[7] == 0x8E) {
            handle_eapol(l + 8, (size_t)(len - 4 - (hlen + 8)), bssid, now);
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// channel hop (core 0 timer)
// ---------------------------------------------------------------------------
static void hop_cb(void *arg)
{
    s_hop_idx = (s_hop_idx + 1) % (sizeof(HOP_SEQ) / sizeof(HOP_SEQ[0]));
    uint8_t ch = HOP_SEQ[s_hop_idx];
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    s_stats.channel = ch;
}

void wifi_sniff_get_stats(wifi_sniff_stats_t *out)
{
    int aps = 0;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < AP_TABLE_N; i++) {
        if (s_aps[i].last_us && (now - s_aps[i].last_us) <= AP_EXPIRE_US) aps++;
    }
    s_stats.aps = aps;
    *out = s_stats;
}

void wifi_sniff_start(QueueHandle_t out_q)
{
    s_out_q = out_q;

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));   // never connect
    ESP_ERROR_CHECK(esp_wifi_start());

    const wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA,
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(&sniff_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(HOP_SEQ[0], WIFI_SECOND_CHAN_NONE));
    s_stats.channel = HOP_SEQ[0];

    const esp_timer_create_args_t targs = {
        .callback = hop_cb, .name = "chhop", .dispatch_method = ESP_TIMER_TASK,
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_hop_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_hop_timer, 300 * 1000));   // 300 ms dwell

    ESP_LOGI(TAG, "promiscuous capture up, hopping 1/6/11 + rest @300ms");
}
