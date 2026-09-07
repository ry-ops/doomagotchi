#include "nav.h"

#include <limits.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

// --- vendored doomgeneric (read-only) ---
#include "doomtype.h"
#include "r_defs.h"
#include "r_state.h"       // sectors, lines, numsectors, numlines
#include "r_main.h"        // R_PointInSubsector
#include "doomdata.h"      // ML_BLOCKING
#include "doomstat.h"      // gameepisode, gamemap
#include "p_local.h"       // P_AproxDistance

static const char *TAG = "nav";

#define PLYR_H     (56 * FRACUNIT)   // MT_PLAYER height - opening must clear this
#define STEP_H     (24 * FRACUNIT)   // max step up (matches P_TryMove)
#define USE_PENALTY 1500             // cost added to a door / lift edge (map units)
#define MAXPATH    96
#define REFRESH    20                // recompute the route at least every ~20 calls

// --- graph (all in PSRAM) ---------------------------------------------------
static int      s_nsec;
static int     *s_head;              // [nsec]  first edge, -1 = none
static fixed_t *s_cx, *s_cy;         // [nsec]  sector centre (bbox origin)

static int     *s_enext;             // [E]     next edge in this sector's list
static int     *s_eto;               // [E]     destination sector
static int     *s_ecost;             // [E]     traversal cost, map units
static fixed_t *s_emx, *s_emy;       // [E]     doorway midpoint
static uint8_t *s_euse;              // [E]     needs a USE to pass
static int      s_ecount;

// --- A* work (PSRAM, sized to nsec) ---------------------------------------
static int     *s_g, *s_came, *s_h;
static uint8_t *s_closed, *s_inopen;

// --- cached route --------------------------------------------------------
static int s_built_e = -1, s_built_m = -1;
static int s_path[MAXPATH];
static int s_plen;
static int s_pto = -1;
static int s_refresh;

// -------------------------------------------------------------------------

static void free_all(void)
{
    void *p[] = { s_head, s_cx, s_cy, s_enext, s_eto, s_ecost, s_emx, s_emy,
                  s_euse, s_g, s_came, s_h, s_closed, s_inopen };
    for (unsigned i = 0; i < sizeof(p) / sizeof(p[0]); i++) {
        heap_caps_free(p[i]);
    }
    s_head = s_cx = s_cy = NULL;
    s_enext = s_eto = s_ecost = NULL;
    s_emx = s_emy = NULL;
    s_euse = s_closed = s_inopen = NULL;
    s_g = s_came = s_h = NULL;
    s_ecount = 0;
    s_plen = 0;
    s_pto = -1;
}

static int sec_of(fixed_t x, fixed_t y)
{
    subsector_t *ss = R_PointInSubsector(x, y);
    if (!ss || !ss->sector) {
        return -1;
    }
    int s = (int)(ss->sector - sectors);
    return (s >= 0 && s < s_nsec) ? s : -1;
}

static void add_edge(int from, int to, int cost, fixed_t mx, fixed_t my, int use)
{
    int e = s_ecount++;
    s_eto[e]   = to;
    s_ecost[e] = cost;
    s_emx[e]   = mx;
    s_emy[e]   = my;
    s_euse[e]  = (uint8_t)use;
    s_enext[e] = s_head[from];
    s_head[from] = e;
}

void nav_build_if_needed(void)
{
    if (s_head && s_built_e == gameepisode && s_built_m == gamemap) {
        return;
    }
    free_all();
    s_built_e = gameepisode;
    s_built_m = gamemap;
    s_nsec = numsectors;
    if (s_nsec <= 0 || !sectors || !lines) {
        return;
    }

    int cap = 2;
    for (int i = 0; i < numlines; i++) {
        if (lines[i].frontsector && lines[i].backsector) {
            cap += 2;
        }
    }

    const int MS = MALLOC_CAP_SPIRAM;
    s_head   = heap_caps_malloc(sizeof(int) * s_nsec, MS);
    s_cx     = heap_caps_malloc(sizeof(fixed_t) * s_nsec, MS);
    s_cy     = heap_caps_malloc(sizeof(fixed_t) * s_nsec, MS);
    s_g      = heap_caps_malloc(sizeof(int) * s_nsec, MS);
    s_came   = heap_caps_malloc(sizeof(int) * s_nsec, MS);
    s_h      = heap_caps_malloc(sizeof(int) * s_nsec, MS);
    s_closed = heap_caps_malloc(s_nsec, MS);
    s_inopen = heap_caps_malloc(s_nsec, MS);
    s_enext  = heap_caps_malloc(sizeof(int) * cap, MS);
    s_eto    = heap_caps_malloc(sizeof(int) * cap, MS);
    s_ecost  = heap_caps_malloc(sizeof(int) * cap, MS);
    s_emx    = heap_caps_malloc(sizeof(fixed_t) * cap, MS);
    s_emy    = heap_caps_malloc(sizeof(fixed_t) * cap, MS);
    s_euse   = heap_caps_malloc(cap, MS);
    if (!s_head || !s_cx || !s_cy || !s_g || !s_came || !s_h || !s_closed ||
        !s_inopen || !s_enext || !s_eto || !s_ecost || !s_emx || !s_emy || !s_euse) {
        ESP_LOGE(TAG, "graph alloc failed (%d sectors, %d edge slots)", s_nsec, cap);
        free_all();
        return;
    }

    for (int i = 0; i < s_nsec; i++) {
        s_head[i] = -1;
        s_cx[i] = sectors[i].soundorg.x;    // bbox centre, set by P_GroupLines
        s_cy[i] = sectors[i].soundorg.y;
    }

    s_ecount = 0;
    for (int i = 0; i < numlines; i++) {
        line_t *L = &lines[i];
        if (!L->frontsector || !L->backsector) {
            continue;
        }
        int a = (int)(L->frontsector - sectors);
        int b = (int)(L->backsector - sectors);
        if (a == b || a < 0 || b < 0 || a >= s_nsec || b >= s_nsec) {
            continue;
        }
        int use = (L->special != 0);
        if ((L->flags & ML_BLOCKING) && !use) {
            continue;                       // rail / impassable two-sided line
        }
        sector_t *sa = &sectors[a], *sb = &sectors[b];
        fixed_t lo_ceil = sa->ceilingheight < sb->ceilingheight ? sa->ceilingheight : sb->ceilingheight;
        fixed_t hi_floor = sa->floorheight  > sb->floorheight  ? sa->floorheight  : sb->floorheight;
        fixed_t opening = lo_ceil - hi_floor;
        boolean tall = use || opening >= PLYR_H;   // a USE-line may be a shut door right now
        if (!tall) {
            continue;
        }
        fixed_t mx = (L->v1->x >> 1) + (L->v2->x >> 1);
        fixed_t my = (L->v1->y >> 1) + (L->v2->y >> 1);
        int cost = (P_AproxDistance(s_cx[a] - mx, s_cy[a] - my) >> FRACBITS)
                 + (P_AproxDistance(s_cx[b] - mx, s_cy[b] - my) >> FRACBITS)
                 + (use ? USE_PENALTY : 0);
        if (use || (sb->floorheight - sa->floorheight) <= STEP_H) {
            add_edge(a, b, cost, mx, my, use);
        }
        if (use || (sa->floorheight - sb->floorheight) <= STEP_H) {
            add_edge(b, a, cost, mx, my, use);
        }
    }

    size_t psram = heap_caps_get_free_size(MS);
    ESP_LOGI(TAG, "graph E%dM%d: %d sectors, %d edges  (psram free %u KiB)",
             gameepisode, gamemap, s_nsec, s_ecount, (unsigned)(psram / 1024));
}

// Dijkstra/A* over the sector graph. Fills s_path[0..s_plen-1] from->to.
static boolean astar(int from, int to)
{
    if (!s_head || from < 0 || to < 0 || from >= s_nsec || to >= s_nsec) {
        return false;
    }
    for (int i = 0; i < s_nsec; i++) {
        s_g[i] = INT_MAX;
        s_came[i] = -1;
        s_closed[i] = 0;
        s_inopen[i] = 0;
        s_h[i] = P_AproxDistance(s_cx[i] - s_cx[to], s_cy[i] - s_cy[to]) >> FRACBITS;
    }
    s_g[from] = 0;
    s_inopen[from] = 1;

    for (;;) {
        int cur = -1, best = INT_MAX;
        for (int i = 0; i < s_nsec; i++) {
            if (!s_inopen[i]) {
                continue;
            }
            int f = s_g[i] + s_h[i];
            if (f < best) { best = f; cur = i; }
        }
        if (cur < 0) {
            return false;                   // open set empty, no route
        }
        if (cur == to) {
            break;
        }
        s_inopen[cur] = 0;
        s_closed[cur] = 1;
        for (int e = s_head[cur]; e >= 0; e = s_enext[e]) {
            int nx = s_eto[e];
            if (s_closed[nx]) {
                continue;
            }
            int ng = s_g[cur] + s_ecost[e];
            if (ng < s_g[nx]) {
                s_g[nx] = ng;
                s_came[nx] = cur;
                s_inopen[nx] = 1;
            }
        }
    }

    int tmp[MAXPATH], n = 0;
    for (int c = to; c >= 0 && n < MAXPATH; c = s_came[c]) {
        tmp[n++] = c;
        if (c == from) {
            break;
        }
    }
    if (n == 0 || tmp[n - 1] != from) {
        return false;                       // path longer than MAXPATH, or broken
    }
    s_plen = n;
    for (int i = 0; i < n; i++) {
        s_path[i] = tmp[n - 1 - i];
    }
    return true;
}

bool nav_waypoint(fixed_t fx, fixed_t fy, fixed_t tx, fixed_t ty,
                  fixed_t *wx, fixed_t *wy, bool *want_use)
{
    *want_use = false;
    *wx = tx;
    *wy = ty;
    if (!s_head) {
        return false;
    }

    int fs = sec_of(fx, fy);
    int ts = sec_of(tx, ty);
    if (fs < 0 || ts < 0) {
        return false;
    }
    if (fs == ts) {
        s_plen = 1;
        s_pto = ts;
        return false;                       // same room - caller steers direct
    }

    boolean need = (ts != s_pto) || s_plen <= 0 || (--s_refresh <= 0);
    if (!need) {
        boolean on = false;
        for (int i = 0; i < s_plen && i < 3; i++) {
            if (s_path[i] == fs) { on = true; break; }
        }
        need = !on;                         // we've drifted off the route
    }
    if (need) {
        if (!astar(fs, ts)) {
            s_plen = 0;
            s_pto = -1;
            return false;
        }
        s_pto = ts;
        s_refresh = REFRESH;
    }

    int i = 0;
    while (i < s_plen && s_path[i] != fs) {
        i++;
    }
    if (i >= s_plen) {
        s_plen = 0;                         // not on the route - recompute next call
        return false;
    }
    while (i + 1 < s_plen && s_path[i + 1] == fs) {
        i++;
    }
    if (i + 1 >= s_plen) {
        return false;                       // in the target's sector already
    }

    int cur = s_path[i], ns = s_path[i + 1];
    int beste = -1, bestd = INT_MAX;
    for (int e = s_head[cur]; e >= 0; e = s_enext[e]) {
        if (s_eto[e] != ns) {
            continue;
        }
        int d = P_AproxDistance(s_emx[e] - fx, s_emy[e] - fy) >> FRACBITS;
        if (d < bestd) { bestd = d; beste = e; }
    }
    if (beste < 0) {
        return false;
    }

    // Aim a bounded ~48 units past the doorway toward the next room, so we
    // actually cross the threshold without the target landing behind a wall in
    // a big room.
    fixed_t mx = s_emx[beste], my = s_emy[beste];
    fixed_t dx = s_cx[ns] - mx, dy = s_cy[ns] - my;
    fixed_t dl = P_AproxDistance(dx, dy);
    if (dl > FRACUNIT) {
        *wx = mx + FixedMul(FixedDiv(dx, dl), 48 * FRACUNIT);
        *wy = my + FixedMul(FixedDiv(dy, dl), 48 * FRACUNIT);
    } else {
        *wx = mx;
        *wy = my;
    }
    *want_use = s_euse[beste];
    return true;
}

int nav_path_len(void)
{
    return s_plen;
}
