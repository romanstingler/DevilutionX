#include "engine/render/visibility_render.hpp"

#include "engine/lighting_defs.hpp"
#include "levels/gendung.h"

namespace devilution {

bool IsTileVisibleToParty(Point tile)
{
	return IsTileVisible(tile);
}

uint8_t ComputeVisibilityLevel(Point tile, bool shadowCullingActive)
{
	if (!shadowCullingActive)
		return 0;
	if (IsTileVisibleToParty(tile))
		return 0;
	return static_cast<uint8_t>(LightsMax);
}

} // namespace devilution