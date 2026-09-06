// autoplay.c — Phase 1 autoplayer.
//
// Drives the player like a human on a keyboard: each frame it reads game state
// (player mobj + the thinker list to find the nearest live monster) and posts
// ev_keydown/ev_keyup through D_PostEvent. G_BuildTiccmd then turns those into
// a ticcmd exactly as it would for real keypresses. No engine edits.
//
// Behaviour: if a live monster is visible-ish, turn toward the nearest one and
// fire; otherwise wander forward with occasional random turns. A stuck check
// (little movement while holding forward) forces a hard turn. Taps USE now and
// then so doors don't trap it. Types "iddqd" once on the first level so an
// unattended run survives the hour.

#include <stdlib.h>

#include "esp_log.h"

// --- vendored doomgeneric ---
#include "doomdef.h"
#include "doomstat.h"
#include "d_event.h"
#include "d_player.h"
#include "p_mobj.h"
#include "p_local.h"
#include "r_main.h"
#include "tables.h"
#include "doomkeys.h"

static const char *TAG = "auto";

#define GODMODE_ON_START 1

// keys we hold; index order matters (see want[] below)
enum { K_FWD, K_LEFT, K_RIGHT, K_FIRE, K_N };
static const unsigned char KEYS[K_N] = {
    KEY_UPARROW, KEY_LEFTARROW, KEY_RIGHTARROW, KEY_FIRE,
};
static boolean s_held[K_N];

#define ALIGNED_ANG   (ANG1 * 12)     // within ~12 deg -> "facing it"
#define FIRE_RANGE     (1400 * FRACUNIT)
#define STANDOFF       (160  * FRACUNIT)
#define STUCK_MOVE     (24   * FRACUNIT)

static void post_key(unsigned char k, boolean down)
{
    event_t ev;
    ev.type = down ? ev_keydown : ev_keyup;
    ev.data1 = k;
    ev.data2 = (k >= ' ' && k < 0x7f) ? k : 0;
    ev.data3 = 0;
    ev.data4 = 0;
    D_PostEvent(&ev);
}

static void apply(const boolean want[K_N])
{
    for (int i = 0; i < K_N; i++) {
        if (want[i] != s_held[i]) {
            post_key(KEYS[i], want[i]);
            s_held[i] = want[i];
        }
    }
}

static void release_all(void)
{
    boolean none[K_N] = { 0 };
    apply(none);
}

// one-shot: type a string as key taps, one char per frame
static const char *s_type;
static boolean s_type_down;
static boolean type_str_step(void)
{
    if (!s_type || !*s_type) {
        s_type = NULL;
        return false;
    }
    if (!s_type_down) {
        post_key((unsigned char)*s_type, true);
        s_type_down = true;
    } else {
        post_key((unsigned char)*s_type, false);
        s_type_down = false;
        s_type++;
    }
    return true;
}

static mobj_t *nearest_monster(mobj_t *me, fixed_t *out_dist)
{
    mobj_t *best = NULL;
    fixed_t bestd = 0x7fffffff;
    for (thinker_t *th = thinkercap.next; th != &thinkercap; th = th->next) {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker) {
            continue;
        }
        mobj_t *m = (mobj_t *)th;
        if (m == me) {
            continue;
        }
        if (!(m->flags & MF_COUNTKILL) || !(m->flags & MF_SHOOTABLE)) {
            continue;
        }
        if (m->health <= 0 || (m->flags & MF_CORPSE)) {
            continue;
        }
        fixed_t d = P_AproxDistance(m->x - me->x, m->y - me->y);
        if (d < bestd) {
            bestd = d;
            best = m;
        }
    }
    if (out_dist) {
        *out_dist = bestd;
    }
    return best;
}

void autoplay_step(void)
{
    static uint32_t tic;
    static fixed_t sx, sy;
    static uint32_t hard_turn_until;
    static uint32_t wander_until;
    static int wander_dir;
    static boolean did_god;
    static boolean enter_down;

    tic++;

    // Not in a level (title / intermission / finale): tap ENTER to advance.
    if (gamestate != GS_LEVEL) {
        release_all();
        enter_down = !enter_down;
        post_key(KEY_ENTER, enter_down);
        return;
    }

    player_t *pl = &players[consoleplayer];
    if (!pl->mo) {
        return;
    }
    if (pl->playerstate == PST_DEAD) {
        release_all();
        enter_down = !enter_down;
        post_key(KEY_ENTER, enter_down);   // respawn
        return;
    }

#if GODMODE_ON_START
    if (!did_god) {
        if (!s_type) {
            s_type = "iddqd";
        }
        if (type_str_step()) {
            return;
        }
        did_god = true;
        ESP_LOGI(TAG, "autoplayer: god mode requested, taking over E1M1");
    }
#endif

    mobj_t *me = pl->mo;
    fixed_t tdist = 0;
    mobj_t *tgt = nearest_monster(me, &tdist);

    boolean want[K_N] = { 0 };

    if (tgt && tdist < FIRE_RANGE) {
        angle_t ta = R_PointToAngle2(me->x, me->y, tgt->x, tgt->y);
        angle_t d = ta - me->angle;          // (0,ANG180) => target is to our left (CCW)
        boolean facing = (d < ALIGNED_ANG) || (d > (angle_t)(0u - ALIGNED_ANG));
        if (!facing) {
            if (d < ANG180) {
                want[K_LEFT] = true;
            } else {
                want[K_RIGHT] = true;
            }
        } else {
            want[K_FIRE] = true;
        }
        if (tdist > STANDOFF) {
            want[K_FWD] = true;
        }
    } else {
        // wander
        want[K_FWD] = true;
        if (tic >= wander_until) {
            wander_dir = rand() % 3;   // 0 straight, 1 left, 2 right
            wander_until = tic + 20 + (rand() % 60);
        }
        if (wander_dir == 1) {
            want[K_LEFT] = true;
        } else if (wander_dir == 2) {
            want[K_RIGHT] = true;
        }
    }

    // stuck? -> hard turn for ~0.7s
    if ((tic & 31) == 0) {
        if (P_AproxDistance(me->x - sx, me->y - sy) < STUCK_MOVE) {
            hard_turn_until = tic + 25;
        }
        sx = me->x;
        sy = me->y;
    }
    if (tic < hard_turn_until) {
        want[K_LEFT] = true;
        want[K_RIGHT] = false;
        want[K_FWD] = true;
    }

    apply(want);

    // periodic USE tap (doors/switches). keydown one frame, keyup the next.
    static boolean use_down;
    if (use_down) {
        post_key(KEY_USE, false);
        use_down = false;
    } else if ((tic % 45) == 0) {
        post_key(KEY_USE, true);
        use_down = true;
    }

    if ((tic % 175) == 0) {
        ESP_LOGI(TAG, "tic=%lu state: %s dist=%d fwd=%d L=%d R=%d fire=%d",
                 (unsigned long)tic, tgt ? "hunting" : "wandering",
                 tgt ? (int)(tdist >> FRACBITS) : -1,
                 s_held[K_FWD], s_held[K_LEFT], s_held[K_RIGHT], s_held[K_FIRE]);
    }
}
