#pragma once

#include <cstdint>

#include "engine/point.hpp"

namespace devilution {

/**
 * @brief Returns true if the tile is within the local party's line of sight,
 *        i.e. no wall tile blocks the ray from the party to `tile`.
 *
 * Unlike `IsTileVisible`, this is independent of the player's vision *radius*:
 * the open floor of a town stays lit while geometry hidden behind a wall is
 * reported as not visible (and culled to black by the renderer).
 */
bool IsTileVisibleToParty(Point tile);

/**
 * @brief Sets the tile used as the origin for line-of-sight checks.
 *
 * Must be called once per frame (before the dungeon is drawn) with the local
 * player's tile. Kept separate from the LOS query so that the visibility
 * module does not depend on the heavy `player.h` include chain.
 */
void SetVisibilityOrigin(Point origin);

/**
 * @brief Per-tile shadow-culling level, in the same 0..15 scale as `dLight`.
 *
 * 0   – tile is fully visible, no extra darkening from shadow culling.
 * 15  – tile is outside the local party's LOS and must render pitch black.
 *
 * Stage 1 (Black) keeps this binary. Stage 2 will add an intermediate value
 * for the explored-memory silhouette.
 */
uint8_t ComputeVisibilityLevel(Point tile, bool shadowCullingActive);

} // namespace devilution