#pragma once

#include <cstdint>

#include "engine/point.hpp"

namespace devilution {

/**
 * @brief Per-tile shadow-culling level used for the explored-memory
 *        silhouette when shadow culling is enabled.
 *
 * 0   – fully visible (no extra darkening).
 * 15  – pitch black (LightsMax).
 * 12  – dim silhouette for explored-but-not-currently-visible tiles.
 */
constexpr uint8_t VisibilityMemoryLevel = 12;

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
 * 0    – tile is fully visible, no extra darkening from shadow culling.
 * 15   – tile is outside the local party's LOS and must render pitch black.
 * 12   – (memory mode) tile has been explored but is not currently
 *         visible, rendered as a dim silhouette.
 *
 * `memoryMode` is the `Graphics.shadowCulling` boolean option: when
 * false the explored-memory silhouette is disabled and hidden tiles render
 * pitch black; when true explored-but-unseen tiles use `VisibilityMemoryLevel`.
 */
uint8_t ComputeVisibilityLevel(Point tile, bool memoryMode);

} // namespace devilution