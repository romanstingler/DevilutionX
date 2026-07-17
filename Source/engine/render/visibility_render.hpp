#pragma once

#include <cstdint>

#include "engine/point.hpp"

namespace devilution {

/**
 * @brief Returns true if any active local-party member has the tile marked
 *        Visible in `dFlags`.
 *
 * In single-player this is equivalent to `IsTileVisible`. In coop, it is the
 * union across all players' vision (already implicit in `dFlags`, which is
 * OR-updated by `ProcessVisionList`).
 */
bool IsTileVisibleToParty(Point tile);

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