#include "engine/render/visibility_render.hpp"

#include <cstdlib>

#include "engine/lighting_defs.hpp"
#include "levels/dun_tile.hpp"
#include "levels/gendung.h"

namespace devilution {

namespace {

// Tile used as the origin for line-of-sight checks. Set every frame by the
// renderer (which has access to the local player) before the dungeon is drawn.
Point gVisibilityOrigin { 0, 0 };

/**
 * @brief Casts a line of sight from `origin` to `tile`.
 *
 * Mirrors the wall-occlusion used by `DoVision` (`TileProperties::BlockLight`):
 * a tile is visible iff no wall tile lies strictly between the origin and it.
 * The endpoint itself may be a wall (so the wall face is still drawn, just not
 * what is behind it).
 *
 * This is deliberately independent of the player's vision *radius* so that, for
 * example, the open floor of a town stays lit while geometry that is actually
 * hidden behind a wall is culled to black.
 */
bool HasLineOfSight(Point origin, Point tile)
{
	if (!InDungeonBounds(tile))
		return false;
	if (!InDungeonBounds(origin))
		return false;

	int x0 = origin.x;
	int y0 = origin.y;
	const int x1 = tile.x;
	const int y1 = tile.y;

	const int dx = std::abs(x1 - x0);
	const int dy = std::abs(y1 - y0);
	const int sx = x0 < x1 ? 1 : -1;
	const int sy = y0 < y1 ? 1 : -1;

	int err = dx - dy;
	for (;;) {
		// Reached the target tile: the line of sight is unobstructed.
		if (x0 == x1 && y0 == y1)
			return true;

		// Check the current tile (excluding the origin, which is always
		// visible) for a light-blocking wall/object.
		if ((x0 != origin.x || y0 != origin.y) && TileHasAny(Point { x0, y0 }, TileProperties::BlockLight))
			return false;

		const int e2 = 2 * err;
		if (e2 > -dy) {
			err -= dy;
			x0 += sx;
		}
		if (e2 < dx) {
			err += dx;
			y0 += sy;
		}
	}
}

} // namespace

void SetVisibilityOrigin(Point origin)
{
	gVisibilityOrigin = origin;
}

bool IsTileVisibleToParty(Point tile)
{
	return HasLineOfSight(gVisibilityOrigin, tile);
}

uint8_t ComputeVisibilityLevel(Point tile, bool memoryMode)
{
	// Town is always fully visible: the open floor and the buildings are
	// meant to be seen in their entirety, and there is no LOS radius
	// gating. This also covers multiplayer (each town is its own
	// DTYPE_TOWN level), so shadow culling is simply disabled there.
	if (leveltype == DTYPE_TOWN)
		return 0;
	if (IsTileVisibleToParty(tile))
		return 0;

	// Tile is outside the local party's line of sight. With the
	// explored-memory silhouette enabled, previously-explored tiles are
	// kept as a dim silhouette instead of pitch black.
	if (memoryMode && InDungeonBounds(tile)
	    && HasAnyOf(dFlags[tile.x][tile.y], DungeonFlag::Explored))
		return VisibilityMemoryLevel;

	return static_cast<uint8_t>(LightsMax);
}

} // namespace devilution
