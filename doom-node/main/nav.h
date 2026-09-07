#pragma once

// nav.c — a coarse navigation graph over the level, for the autoplayer.
//
// Nodes are sectors; edges are the two-sided linedefs between them that a
// player could actually cross (tall enough opening, <=24 step up, or a door /
// lift line that a USE will open). A* over that graph turns "the monster is
// three rooms away" into a list of doorways to walk through.
//
// Everything here is read-only queries into the engine's own map data
// (sectors[], lines[], R_PointInSubsector) - no game-logic edits (ADR 0001).

#include <stdbool.h>

#include "m_fixed.h"

// (Re)build the graph if the level changed. Cheap to call every frame - it
// only does work on a new E?M?.
void nav_build_if_needed(void);

// The next point to steer toward on a routed path from (fx,fy) to (tx,ty).
// Returns false when there's no route (same sector, or genuinely unreachable) -
// the caller then just steers straight at the target. *want_use is set when the
// doorway we're approaching needs a USE (door or lift).
bool nav_waypoint(fixed_t fx, fixed_t fy, fixed_t tx, fixed_t ty,
                  fixed_t *wx, fixed_t *wy, bool *want_use);

// Length (in sectors) of the currently cached route; 0 if none. For the log.
int nav_path_len(void);
