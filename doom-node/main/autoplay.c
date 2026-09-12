// autoplay.c — the autoplayer.
//
// Drives the player like a human on a keyboard: each frame it reads game state
// and posts ev_keydown/ev_keyup through D_PostEvent, so G_BuildTiccmd turns
// them into a ticcmd exactly as it would for real keypresses. It never touches
// game logic (ADR 0001 / CLAUDE.md constraint #3).
//
// Loop each frame:
//   1. acquire()  - pick a target. A monster we can see always wins; otherwise
//                   keep chasing the last one's position for a few seconds,
//                   otherwise head for the nearest monster anywhere.
//   2. aim at it. pick_dir() nudges the aim around a blocked straight line
//      (P_CheckPosition probes, read-only) but never stops us moving - DOOM's
//      own wall-sliding in P_XYMovement carries us along grazed walls.
//   3. hold forward while out of range, ease back inside the standoff, strafe
//      toward the target while turning so corners get rounded.
//   4. pick a weapon for the monster's type and fire once we can see it and
//      we're pointed roughly at it.
//   5. if we're wedged (position genuinely not changing) do a short "kick" -
//      turn + strafe + forward, alternating side - and tap USE for doors.
//
// Types iddqd + idkfa once so an unattended run survives; idfa periodically.

#include <stdlib.h>
#include <stdint.h>
#include <limits.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "autoplay.h"
#include "nav.h"

// --- vendored doomgeneric ---
#include "doomdef.h"
#include "doomstat.h"
#include "d_event.h"
#include "d_player.h"
#include "p_mobj.h"
#include "p_local.h"
#include "info.h"
#include "m_fixed.h"
#include "r_main.h"
#include "tables.h"
#include "doomkeys.h"

static const char *TAG = "auto";

#define GODMODE_ON_START 1
#define HUMAN_HOLDOFF_US  (5 * 1000 * 1000)   // stand down 5 s after a human keypress
#define KEEPALIVE_SPAWN   1   // no kill in ~30 s -> drop an imp in front (stand-in
                              // for the Phase 3 RF feed; same P_SpawnMobj path)

// Far enough in the past that we're not suspended at boot, but not so far that
// (now - s_human_us) can overflow.
static volatile int64_t s_human_us = -100000000;   // -100 s

void autoplay_note_human_key(void)
{
    s_human_us = esp_timer_get_time();
}

bool autoplay_suspended(void)
{
    return (esp_timer_get_time() - s_human_us) < HUMAN_HOLDOFF_US;
}

// keys we hold; index order matches KEYS[] and want[] below
enum { K_FWD, K_BACK, K_LEFT, K_RIGHT, K_SL, K_SR, K_FIRE, K_N };
static const unsigned char KEYS[K_N] = {
    KEY_UPARROW, KEY_DOWNARROW, KEY_LEFTARROW, KEY_RIGHTARROW,
    KEY_STRAFE_L, KEY_STRAFE_R, KEY_FIRE,
};
static boolean s_held[K_N];

#define PROBE         (28  * FRACUNIT)  // aim-nudge look-ahead (~player radius + a bit)
#define STEP_UP_MAX   (24  * FRACUNIT)  // matches P_TryMove's climb limit
#define TURN_DEADBAND (ANG1 * 8)        // within this of the heading = "facing"
#define AHEAD_CONE    (ANG1 * 70)       // heading within this -> ok to hold forward
#define FIRE_CONE     (ANG1 * 14)       // pointed within this of the target -> fire
#define FIRE_GATE     (900 * FRACUNIT)  // ... and this close (sight is unreliable here)
#define STANDOFF      (200  * FRACUNIT)
#define LOST_GRACE    75               // tics to chase a target we can't see (~4-5s)
#define KICK_TICS     20               // wedge-escape burst (~1.2s)
#define KICK_GIVEUP   3               // kicks with no sight -> blacklist, move on
#define BLACKLIST_T   150             // ignore an unreachable target for ~10s

// ------------------------------------------------------------------ keys ---

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

// USE tap spread over two frames (keydown, then keyup)
static void use_tick(uint32_t tic, uint32_t period)
{
    static boolean down;
    if (down) {
        post_key(KEY_USE, false);
        down = false;
    } else if ((tic % period) == 0) {
        post_key(KEY_USE, true);
        down = true;
    }
}

// ------------------------------------------------------------- navigation ---

// Is the cell at (x,y) somewhere the player could actually stand and step to?
// P_CheckPosition is DOOM's own collision test; tmfloorz/tmceilingz are the
// globals it leaves behind. We only read them.
static boolean cell_ok(mobj_t *me, fixed_t x, fixed_t y)
{
    if (!P_CheckPosition(me, x, y)) {
        return false;
    }
    if (tmfloorz - me->z > STEP_UP_MAX) {
        return false;                    // impassable step up
    }
    if (tmceilingz - tmfloorz < me->height) {
        return false;                    // no headroom
    }
    return true;
}

static boolean dir_ok(mobj_t *me, angle_t a, fixed_t dist)
{
    unsigned fa = a >> ANGLETOFINESHIFT;
    return cell_ok(me,
                   me->x + FixedMul(dist, finecosine[fa]),
                   me->y + FixedMul(dist, finesine[fa]));
}

// Aim: the straight-line bearing to the target if that path is clear, else the
// nearest open 45-degree step off it (P_NewChaseDir's idea). Advisory only - the
// caller still holds forward and lets P_XYMovement slide us along walls.
static angle_t pick_dir(mobj_t *me, angle_t bearing)
{
    if (dir_ok(me, bearing, PROBE)) {
        return bearing;
    }
    static const int off[4] = { 1, -1, 2, -2 };
    for (int i = 0; i < 4; i++) {
        angle_t a = bearing + (angle_t)off[i] * ANG45;
        if (dir_ok(me, a, PROBE)) {
            return a;
        }
    }
    return bearing;
}

// ------------------------------------------------------------------ weapons ---

// A weapon per monster type - loosely themed on the RF->enemy table in CLAUDE.md.
// Returns the number-row key char DOOM binds to that weapon ('1'..'7'), or 0 to
// leave the current weapon alone.
static int weap_for(mobjtype_t t)
{
    switch (t) {
    case MT_POSSESSED: return '2';   // Zombieman     -> pistol
    case MT_SHOTGUY:   return '3';   // Shotgun Guy    -> shotgun
    case MT_TROOP:     return '4';   // Imp            -> chaingun
    case MT_SERGEANT:                // Demon
    case MT_SHADOWS:   return '3';   // Spectre        -> shotgun, point blank
    case MT_SKULL:     return '4';   // Lost Soul      -> chaingun
    case MT_HEAD:      return '6';   // Cacodemon      -> plasma
    case MT_PAIN:      return '6';   // Pain Elemental -> plasma
    case MT_KNIGHT:                  // Hell Knight
    case MT_BRUISER:   return '5';   // Baron of Hell  -> rockets
    case MT_UNDEAD:    return '4';   // Revenant       -> chaingun
    case MT_FATSO:     return '5';   // Mancubus       -> rockets
    case MT_BABY:      return '6';   // Arachnotron    -> plasma
    case MT_VILE:      return '6';   // Archvile       -> plasma
    case MT_WOLFSS:    return '2';   // Wolfenstein SS -> pistol
    case MT_CYBORG:                  // Cyberdemon
    case MT_SPIDER:    return '7';   // Spider Mastermind -> BFG
    default:           return '3';   // anything else  -> shotgun
    }
}

// One-shot weapon switch: tap the key the frame the desired weapon changes
// (keydown one frame, keyup the next). G_BuildTiccmd turns that into BT_CHANGE.
static void select_weapon(int wantchar)
{
    static int held;
    static int committed;
    if (held) {
        post_key((unsigned char)held, false);
        held = 0;
        return;
    }
    if (wantchar && wantchar != committed) {
        post_key((unsigned char)wantchar, true);
        held = wantchar;
        committed = wantchar;
    }
}

static boolean aim_hits_monster(void)
{
    return linetarget && (linetarget->flags & MF_SHOOTABLE) &&
           linetarget->health > 0 && !(linetarget->flags & MF_CORPSE) &&
           (linetarget->flags & MF_COUNTKILL);
}

// Sweep DOOM's own autoaim trace across a wide arc. It stops at walls, so a hit
// is a real, unobstructed shot - the gate P_CheckSight can't give us in this
// IWAD. Returns the monster nearest to straight-ahead and, via *rel, the signed
// angle to turn to line it up.
static mobj_t *scan_target(mobj_t *me, angle_t *rel)
{
    mobj_t *best = NULL;
    int best_mag = 999;
    for (int deg = -54; deg <= 54; deg += 6) {
        P_AimLineAttack(me, me->angle + (angle_t)deg * ANG1, 2400 * FRACUNIT);
        if (aim_hits_monster() && linetarget != me) {
            int mag = deg < 0 ? -deg : deg;
            if (mag < best_mag) {
                best_mag = mag;
                best = linetarget;
                *rel = (angle_t)deg * ANG1;
            }
        }
    }
    return best;
}

// ---------------------------------------------------------------- target ---

static mobj_t *s_target;                 // locked-on monster (validated each frame)
static fixed_t s_tx, s_ty, s_tz;         // its position
static uint32_t s_seen_tic;              // last frame we had sight of it
static boolean s_visible;                // can shoot it this frame

static mobj_t *s_blacklist;              // a target we bailed on
static uint32_t s_blacklist_until;

// diagnostics, filled by acquire()
static int s_dbg_nhunt;                  // huntable monsters alive
static int s_dbg_near_d;                 // nearest one's distance (map units)
static boolean s_dbg_near_sight;         // ... and whether P_CheckSight reaches it

static boolean huntable(mobj_t *m, mobj_t *me)
{
    if (m == me) {
        return false;
    }
    if (!(m->flags & MF_COUNTKILL) || !(m->flags & MF_SHOOTABLE)) {
        return false;
    }
    if (m->health <= 0 || (m->flags & MF_CORPSE)) {
        return false;
    }
    return true;
}

// One pass over the thinker list -> the nearest live monster, which we lock and
// steer toward. (P_CheckSight is unreliable in the Freedoom IWAD - its REJECT
// lump rejects almost everything - so we can't gate on line of sight; we track
// it for the log only and decide "can I shoot" from range + facing instead.)
static void acquire(mobj_t *me, uint32_t tic)
{
    mobj_t *near_any = NULL, *found = NULL;
    fixed_t d_any = 0x7fffffff;
    int nhunt = 0;

    for (thinker_t *th = thinkercap.next; th != &thinkercap; th = th->next) {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker) {
            continue;
        }
        mobj_t *m = (mobj_t *)th;
        if (!huntable(m, me)) {
            continue;
        }
        nhunt++;
        if (m == s_target) {
            found = m;
        }
        if (m == s_blacklist && tic < s_blacklist_until) {
            continue;
        }
        // Cost, not raw distance: a monster far below/above (in a pit, on a
        // ledge) is hard to reach and hard to shoot, so weight the height gap
        // heavily. Keeps the bot on same-level targets unless a vertical one is
        // the only thing left.
        fixed_t d = P_AproxDistance(m->x - me->x, m->y - me->y);
        fixed_t dz = me->z - m->z;
        if (dz < 0) { dz = -dz; }
        fixed_t cost = d + (dz << 2);
        if (cost < d_any) {
            d_any = cost;
            near_any = m;
            s_dbg_near_sight = P_CheckSight(me, m);
        }
    }
    s_dbg_nhunt = nhunt;
    s_dbg_near_d = near_any ? (int)(P_AproxDistance(near_any->x - me->x,
                                                   near_any->y - me->y) >> FRACBITS) : -1;

    if (s_target && !found) {
        s_target = NULL;                  // it died / was removed
    }
    // Stick with the current lock unless something else is clearly cheaper.
    if (near_any) {
        fixed_t tc = 0x7fffffff;
        if (s_target) {
            fixed_t tdz = me->z - s_target->z;
            if (tdz < 0) { tdz = -tdz; }
            tc = P_AproxDistance(s_target->x - me->x, s_target->y - me->y) + (tdz << 2);
        }
        if (!s_target || d_any + (d_any >> 2) < tc) {
            s_target = near_any;
        }
    }
    if (s_target) {
        s_tx = s_target->x;
        s_ty = s_target->y;
        s_tz = s_target->z;
        s_seen_tic = tic;
    }
}

static void bail_target(uint32_t tic)
{
    if (s_target) {
        s_blacklist = s_target;
        s_blacklist_until = tic + BLACKLIST_T;
        ESP_LOGI(TAG, "tic=%lu: target unreachable, blacklisting ~10s", (unsigned long)tic);
    }
    s_target = NULL;
}

static void dbg_log(uint32_t tic, const char *mode, int gap, int tgt_type)
{
    if ((tic % 105) != 0) {
        return;
    }
    ESP_LOGI(TAG, "tic=%lu %-6s gap=%d vis=%d type=%d | fwd=%d bk=%d L=%d R=%d SL=%d SR=%d fire=%d "
                  "| nhunt=%d near=%d route=%d",
             (unsigned long)tic, mode, gap, s_visible, tgt_type,
             s_held[K_FWD], s_held[K_BACK], s_held[K_LEFT], s_held[K_RIGHT],
             s_held[K_SL], s_held[K_SR], s_held[K_FIRE],
             s_dbg_nhunt, s_dbg_near_d, nav_path_len());
}

// ------------------------------------------------------------------ step ---

void autoplay_step(void)
{
    static uint32_t tic;
    static uint32_t wander_until;
    static int wander_dir;
    static boolean enter_down;

    // wedge escape
    static uint32_t kick_until;
    static int kick_side;                 // +1 / -1, alternates each kick
    static int kick_count;

    // give-up-on-unreachable-target progress tracking
    static fixed_t best_gap = 0x7fffffff;
    static uint32_t progress_tic;
    static mobj_t *gap_for;

    // coarse "am I physically moving" check
    static fixed_t ox, oy;
    static uint32_t moved_tic;

    tic++;

    // Human at the keyboard within the last few seconds -> get out of the way
    // entirely: release our keys and post nothing, so cheat codes and manual
    // steering land cleanly. Resumes on its own once the keyboard goes quiet.
    static boolean was_suspended;
    if (autoplay_suspended()) {
        if (!was_suspended) {
            release_all();
            select_weapon(0);
            s_type = NULL;                 // drop any half-typed auto-cheat
            ESP_LOGI(TAG, "human control - autoplayer standing down");
            was_suspended = true;
        }
        return;
    }
    if (was_suspended) {
        ESP_LOGI(TAG, "keyboard quiet - autoplayer resuming");
        was_suspended = false;
    }

    // First boot: let the title screen / demo attract loop actually show and
    // wait for a real human keypress to start a game (DOOM behaving like DOOM).
    // Once a game has been started at least once, this no longer applies -
    // every later trip through the menu/intermission (death, level exit, ESC)
    // gets the usual auto-Enter so unattended self-play keeps working.
    //
    // demoplayback (the built-in demo1/2/3 lumps) is its own trap: replaying a
    // recorded demo re-simulates a real level, so gamestate reads GS_LEVEL the
    // whole time. Always stand fully down for it - never latch on it, never
    // feed it input - so the attract loop plays out untouched, same as a human
    // just watching real DOOM idle.
    static boolean s_game_ever_started;
    if (gamestate == GS_LEVEL && !demoplayback) {
        s_game_ever_started = true;
    }
    if (demoplayback) {
        release_all();
        return;
    }
    if (gamestate != GS_LEVEL) {
        release_all();
        if (!s_game_ever_started) {
            return;   // hands off - a human has to press something to start
        }
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
        post_key(KEY_ENTER, enter_down);
        return;
    }

#if GODMODE_ON_START
    // Cheats are re-entered on every level start and periodically thereafter -
    // the typing mechanism is always live, never a one-shot. (A human at the
    // Tab5 keyboard can also type any cheat at any time; same event path.)
    static int cheat_e = -1, cheat_m = -1;   // last level we godded
    static int cheat_step;                   // 1 = owe iddqd, 2 = owe idkfa, 0 = done
    if (gameepisode != cheat_e || gamemap != cheat_m) {
        cheat_e = gameepisode;
        cheat_m = gamemap;
        cheat_step = 1;
    }
    if (cheat_step && !s_type) {
        s_type = (cheat_step == 1) ? "iddqd" : "idkfa";
        cheat_step = (cheat_step == 1) ? 2 : 0;
        if (cheat_step == 0) {
            ESP_LOGI(TAG, "autoplayer: cheats (re)entered for E%dM%d", gameepisode, gamemap);
        }
    }
    if (cheat_step == 0 && !s_type && (tic % 2000) == 0) {
        s_type = "idfa";                  // periodic ammo / armour top-up
    }
    if (s_type && type_str_step()) {
        return;
    }
#endif

    mobj_t *me = pl->mo;
    nav_build_if_needed();
    acquire(me, tic);

#if KEEPALIVE_SPAWN
    // Nothing has died in a while -> the reachable monsters are cleared (or all
    // that's left is walled-off pits). Drop a fresh imp in front of the player
    // so the "gameplay" keeps going. This is exactly the P_SpawnMobj call the
    // Phase 3 RX will make on AP_SEEN - just triggered by boredom for now.
    {
        static int last_nhunt = -1;
        static uint32_t last_kill_tic, last_spawn_tic;
        if (last_nhunt < 0) { last_nhunt = s_dbg_nhunt; last_kill_tic = tic; }
        if (s_dbg_nhunt < last_nhunt) { last_kill_tic = tic; }
        last_nhunt = s_dbg_nhunt;
        if (tic - last_kill_tic > 450 && tic - last_spawn_tic > 300) {
            unsigned fa = me->angle >> ANGLETOFINESHIFT;
            fixed_t sx = me->x + FixedMul(220 * FRACUNIT, finecosine[fa]);
            fixed_t sy = me->y + FixedMul(220 * FRACUNIT, finesine[fa]);
            if (P_CheckPosition(me, sx, sy) &&
                P_SpawnMobj(sx, sy, ONFLOORZ, MT_TROOP)) {
                ESP_LOGI(TAG, "tic=%lu keepalive: spawned an imp", (unsigned long)tic);
            }
            last_spawn_tic = tic;
            last_kill_tic = tic;
        }
    }
#endif

    // physical-movement sample (every 16 tics)
    if ((tic & 15) == 0) {
        if (P_AproxDistance(me->x - ox, me->y - oy) > (24 * FRACUNIT)) {
            moved_tic = tic;
        }
        ox = me->x;
        oy = me->y;
    }
    boolean not_moving = (tic - moved_tic) > 22;   // ~1.3s barely moving = wedged

    boolean want[K_N] = { 0 };
    const char *mode = "hunt";

    if (!s_target) {
        // level clear-ish: wander, poke switches, unwedge with a hard turn
        mode = "wander";
        want[K_FWD] = true;
        if (tic >= wander_until) {
            wander_dir = rand() % 3;
            wander_until = tic + 20 + (rand() % 60);
        }
        if (wander_dir == 1) { want[K_LEFT] = true; }
        else if (wander_dir == 2) { want[K_RIGHT] = true; }
        if (not_moving) { want[K_LEFT] = true; want[K_FWD] = false; }
        select_weapon(0);
        use_tick(tic, 18);
        apply(want);
        dbg_log(tic, mode, -1, -1);
        return;
    }

    // --- SHOOT: hit anything DOOM's own autoaim can reach in a wide arc ----
    angle_t srel = 0;
    mobj_t *scan = scan_target(me, &srel);
    if (scan) {
        mode = "shoot";
        boolean sleft = (srel != 0) && (srel < ANG180);
        angle_t smag = sleft ? srel : (angle_t)(0u - srel);
        fixed_t sd = P_AproxDistance(scan->x - me->x, scan->y - me->y);
        if (smag > TURN_DEADBAND) {
            want[sleft ? K_LEFT : K_RIGHT] = true;
        }
        if (smag < FIRE_CONE) {
            int wk = weap_for(scan->type);
            if ((wk == '5' || wk == '7') && sd < (360 * FRACUNIT)) {
                wk = '4';                    // no rockets / BFG point blank
            }
            select_weapon(wk);
            want[K_FIRE] = true;
        } else {
            select_weapon(0);
        }
        if (sd > STANDOFF && smag < AHEAD_CONE) {
            want[K_FWD] = true;
        } else if (sd < STANDOFF - (64 * FRACUNIT)) {
            want[K_BACK] = true;
        }
        s_visible = true;
        moved_tic = tic;                     // trading fire in place isn't wedged
        kick_count = 0;
        apply(want);
        dbg_log(tic, mode, (int)(sd >> FRACBITS), (int)scan->type);
        return;
    }
    s_visible = false;

    // --- NAVIGATE: nothing to shoot - route toward the target through the map -
    fixed_t gap = P_AproxDistance(s_tx - me->x, s_ty - me->y);
    angle_t mon_bearing = R_PointToAngle2(me->x, me->y, s_tx, s_ty);

    // Route through the sector graph. If it gives a waypoint we steer at that
    // (the next doorway); otherwise (same room / unreachable) steer at the
    // monster and let the reactive fallback below handle it.
    fixed_t wx, wy;
    bool nav_use = false;
    boolean routed = nav_waypoint(me->x, me->y, s_tx, s_ty, &wx, &wy, &nav_use);
    angle_t bearing = routed ? R_PointToAngle2(me->x, me->y, wx, wy) : mon_bearing;

    // No progress for a while -> unreachable from here; blacklist it and let
    // acquire() pick another so we don't grind one wall or one pit rim.
    if (gap_for != s_target) {
        gap_for = s_target;
        best_gap = 0x7fffffff;
        progress_tic = tic;
    }
    if (gap + (48 * FRACUNIT) < best_gap) {
        best_gap = gap;
        progress_tic = tic;
    } else if (tic - progress_tic > 120) {   // ~8 s of no ground gained
        bail_target(tic);
        best_gap = 0x7fffffff;
        progress_tic = tic;
        release_all();
        return;
    }

    // Target well below us and close in 2D -> we're on a ledge / pit rim above
    // it. There's nothing to shoot (autoaim can't angle down that far) and the
    // rim reads as an obstacle - so just walk straight off toward it and drop
    // to its level. Don't treat the rim as a door.
    boolean below = (me->z - s_tz) > (56 * FRACUNIT);
    if (below && gap < (280 * FRACUNIT)) {
        // Try for ~3 s to walk off the rim; if we still haven't dropped (many
        // Freedoom pits are fully walled - you can't get in from above), give
        // up on this one.
        static uint32_t drop_since;
        static mobj_t *drop_for;
        if (drop_for != s_target) { drop_for = s_target; drop_since = tic; }
        if (tic - drop_since > 45 && (me->z - s_tz) > (56 * FRACUNIT)) {
            bail_target(tic);
            release_all();
            return;
        }
        mode = "drop";
        angle_t da = mon_bearing - me->angle;
        boolean left = (da != 0) && (da < ANG180);
        if (!(da < TURN_DEADBAND || da > (angle_t)(0u - TURN_DEADBAND))) {
            want[left ? K_LEFT : K_RIGHT] = true;
        }
        want[K_FWD] = true;
        if (not_moving) {                     // rim has a rail - slide along it
            want[kick_side >= 0 ? K_SR : K_SL] = true;
        }
        select_weapon(0);
        apply(want);
        dbg_log(tic, mode, (int)(gap >> FRACBITS), s_target ? (int)s_target->type : -1);
        return;
    }

    // Straight path blocked? A door / lift / riser is likely in the way - keep
    // shoving into it and lean on USE.
    boolean obstacle = !dir_ok(me, bearing, PROBE);

    boolean kicking = tic < kick_until;
    if (not_moving && !kicking) {
        kick_side = (kick_side >= 0) ? -1 : 1;
        kick_until = tic + KICK_TICS;
        kicking = true;
        if (++kick_count >= KICK_GIVEUP) {
            bail_target(tic);
            kick_count = 0;
            release_all();
            return;
        }
    }
    if (!not_moving && tic > kick_until + 30) {
        kick_count = 0;
    }

    if (kicking) {
        mode = "kick";
        want[K_FWD] = true;
        if (kick_side > 0) { want[K_LEFT] = true;  want[K_SL] = true; }
        else               { want[K_RIGHT] = true; want[K_SR] = true; }
        use_tick(tic, 8);
    } else {
        mode = routed ? "route" : "chase";
        angle_t aim = obstacle ? bearing : pick_dir(me, bearing);
        angle_t da = aim - me->angle;
        boolean facing = (da < TURN_DEADBAND) || (da > (angle_t)(0u - TURN_DEADBAND));
        boolean left = (da != 0) && (da < ANG180);
        if (!facing) {
            want[left ? K_LEFT : K_RIGHT] = true;
            want[left ? K_SL : K_SR] = true;
        }
        boolean ahead = (da < AHEAD_CONE) || (da > (angle_t)(0u - AHEAD_CONE));
        // routed: keep moving toward the doorway through a wider cone (the
        // turn + strafe swing us onto it); chasing: only push when facing it.
        angle_t wide = AHEAD_CONE + ANG45;
        boolean push = (da < wide) || (da > (angle_t)(0u - wide));
        if (obstacle || nav_use || (routed && push) || (gap > STANDOFF && ahead)) {
            want[K_FWD] = obstacle || nav_use || (routed ? push : ahead);
        }
        use_tick(tic, (obstacle || nav_use) ? 8 : 40);
    }

    select_weapon(0);
    apply(want);
    dbg_log(tic, mode, (int)(gap >> FRACBITS),
            s_target ? (int)s_target->type : -1);
}
