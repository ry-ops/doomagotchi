#include "mood.h"

#include <string.h>
#include "esp_timer.h"

// ---- sliding-window ring buffers of event timestamps (us) ----------------
#define AP_WIN_US   (60LL * 1000 * 1000)
#define CAP_WIN_US  (120LL * 1000 * 1000)
#define AP_RING     64
#define CAP_RING    32

static int64_t s_ap_ts[AP_RING];
static int64_t s_cap_ts[CAP_RING];
static uint8_t s_ap_head, s_cap_head;

static int64_t s_last_ap_us;
static int64_t s_last_cap_us;

static mood_t  s_mood;
static uint8_t s_intensity;
static uint32_t s_quip_seq;      // bumped whenever the mood changes -> new quip

static const char *LABEL[MOOD_COUNT] = {
    "BORED", "RESTLESS", "HUNTING", "MANIC", "RAMPAGE",
};

// A few lines per mood; mood.c rotates through them on each mood change.
static const char *QUIPS[MOOD_COUNT][4] = {
    [MOOD_BORED] = {
        "the airwaves are silent. i hunger.",
        "no signal. no sport. no point.",
        "dead channel. dead time.",
        "wake me when something transmits.",
    },
    [MOOD_RESTLESS] = {
        "beacons, but no blood.",
        "a few of them out there. not enough.",
        "pacing. waiting. sharpening.",
        "come closer. associate. i dare you.",
    },
    [MOOD_HUNTING] = {
        "targets acquired. all of them.",
        "the spectrum is crawling tonight.",
        "one by one. i have time.",
        "steady work. good work.",
    },
    [MOOD_MANIC] = {
        "another one bleeds four-way.",
        "handshake down. who's next.",
        "i can smell the EAPOL.",
        "that PMKID never stood a chance.",
    },
    [MOOD_RAMPAGE] = {
        "they keep associating. they keep dying.",
        "RIP AND TEAR through the RSN.",
        "the whole SSID list is on fire.",
        "more. MORE.",
    },
};

static uint32_t window_count(const int64_t *ring, int n, int64_t win, int64_t now)
{
    uint32_t c = 0;
    for (int i = 0; i < n; i++) {
        if (ring[i] && (now - ring[i]) <= win) c++;
    }
    return c;
}

void mood_init(void)
{
    memset(s_ap_ts, 0, sizeof(s_ap_ts));
    memset(s_cap_ts, 0, sizeof(s_cap_ts));
    s_ap_head = s_cap_head = 0;
    s_last_ap_us = s_last_cap_us = 0;
    s_mood = MOOD_BORED;
    s_intensity = 0;
    s_quip_seq = 0;
}

void mood_on_event(const struct dgm_event *ev)
{
    int64_t now = esp_timer_get_time();
    switch (ev->event_type) {
    case DGM_EV_AP_SEEN:
        s_ap_ts[s_ap_head] = now;
        s_ap_head = (s_ap_head + 1) % AP_RING;
        s_last_ap_us = now;
        break;
    case DGM_EV_HANDSHAKE:
    case DGM_EV_PMKID:
        s_cap_ts[s_cap_head] = now;
        s_cap_head = (s_cap_head + 1) % CAP_RING;
        s_last_cap_us = now;
        break;
    default:
        break;
    }
}

void mood_tick(void)
{
    int64_t now = esp_timer_get_time();
    uint32_t aps = window_count(s_ap_ts, AP_RING, AP_WIN_US, now);
    uint32_t caps = window_count(s_cap_ts, CAP_RING, CAP_WIN_US, now);
    int64_t since_ap  = s_last_ap_us  ? (now - s_last_ap_us)  : INT64_MAX;
    int64_t since_cap = s_last_cap_us ? (now - s_last_cap_us) : INT64_MAX;

    mood_t m;
    if (since_cap <= 20LL * 1000 * 1000 && caps >= 2) {
        m = MOOD_RAMPAGE;
    } else if (since_cap <= 45LL * 1000 * 1000) {
        m = MOOD_MANIC;
    } else if (since_ap <= 30LL * 1000 * 1000 && aps >= 3) {
        m = MOOD_HUNTING;
    } else if (since_ap <= 180LL * 1000 * 1000) {
        m = MOOD_RESTLESS;
    } else {
        m = MOOD_BORED;
    }

    // intensity 0..255 = "how much blood is in the air". Dominated by recent
    // captures; AP churn is a small saturating background term so a dense but
    // quiet area idles warm, not maxed.
    int heat = 0;
    if (since_cap != INT64_MAX) {
        int64_t s = since_cap / 1000000;
        if (s < 60) heat = (int)(210 - s * 3);   // 210 -> ~30 over a minute
    }
    int ap_term = (int)(aps > 20 ? 20 : aps) * 3;   // 0..60, saturating
    heat += ap_term + (int)caps * 30;
    if (heat < 0) heat = 0;
    if (heat > 255) heat = 255;

    if (m != s_mood) {
        s_mood = m;
        s_quip_seq++;
    }
    s_intensity = (uint8_t)heat;
}

void mood_get(mood_state_t *out)
{
    int64_t now = esp_timer_get_time();
    out->mood = s_mood;
    out->intensity = s_intensity;
    out->label = LABEL[s_mood];
    out->quip = QUIPS[s_mood][s_quip_seq & 3];
    out->aps_1m = window_count(s_ap_ts, AP_RING, AP_WIN_US, now);
    out->caps_2m = window_count(s_cap_ts, CAP_RING, CAP_WIN_US, now);
}
