# Plan: Line-of-Sight Shadow Culling (Issue #780)

Add Diablo II / Lord-of-Terror style "what the player can actually see" rendering to DevilutionX without changing the renderer backend. All work stays in the existing 8-bit indexed CPU pipeline used by SDL1, SDL2, SDL3, and headless mode; no `SDL_GPU*` is required.

## Goal

Hide tiles outside the local party's line of sight when rendering the dungeon, so that wall geometry correctly occludes the world. First stage ships a hard-black tile-level mask. Later stages add D2-style explored memory and a soft per-pixel fade.

## Constraints

- No `SDL_GPU*`, no new renderer backend, no 32-bit color pipeline.
- LOS data must not be mutated; rendering must remain a pure function of `dFlags`, `dLight`, and `dPiece`.
- Visibility changes must be render-only. They must not influence AI, automap memory, item selection, or save format.
- Must work in coop (party-OR visibility), in town (full visibility), with infravision, and in Hellfire `DTYPE_NEST`/`DTYPE_CRYPT`/`DTYPE_HELL` where `FullyDarkLightTable` may be null.
- Must integrate with both `perPixelLighting` off (tile-LUT path) and on (per-pixel path) without special-casing each `LightType` template.

## Affected Boundaries

| Boundary | Files | Notes |
| --- | --- | --- |
| LOS data | `Source/lighting.cpp`, `Source/levels/gendung.h`, `Source/vision.cpp` | Already correct; reading only. No mutation. |
| Per-tile light value | `Source/engine/render/scrollrt.cpp:680, :782, :1071` | Wrap the existing `dLight[...]` reads. |
| Per-pixel lightmap | `Source/engine/render/light_render.{hpp,cpp}`, `Source/engine/render/blit_impl.hpp` | Add parallel visibility channel; combine with light level via `max()` inside `BlitPixelsWithLightmap` / `BlitPixelsBlendedWithLightmap`. |
| Entity gating | `Source/engine/render/scrollrt.cpp` (DrawMonsterHelper, DrawPlayer, DrawMissile, DrawObject, DrawItem, DrawDeadPlayer) | Out of scope for Stage 1; tile geometry is enough to show the silhouettes. |
| Options / settings | `Source/options.h`, `Source/options.cpp`, `Source/lua/modules/dev/display.cpp`, `Source/debug.{h,cpp}` | New enum + debug toggle mirroring `DebugVision`. |
| Save format | `Source/loadsave.cpp` | None: shadow-culling mode is a regular option, persisted like other graphics options. |
| Tests | `test/vision_test.cpp`, `test/light_render_benchmark.cpp` | Extend with a shadow-culling scenario; benchmark must remain green. |

## Data Flow

1. `ProcessVisionList` (`Source/lighting.cpp:546-584`) writes per-player `DungeonFlag::Visible | Lit | Explored` into `dFlags` on each game-logic tick. No changes here.
2. New helper `IsTileVisibleToParty(Point p)` returns true if any active player (in coop) has marked the tile `Visible`. For single player this is equivalent to the existing `IsTileVisible`. Lives in `Source/engine/render/visibility_render.hpp/cpp`.
3. New `Options.Graphics.shadowCulling` enum: `Off | Black | Memory`. Default `Off` (preserves current behavior; feature-flag friendly).
4. `DrawGame` (`Source/engine/render/scrollrt.cpp:1274`) calls a new `BuildVisibilityMap(position, offset, viewportWidth, viewportHeight, rows, columns, visibilityMapBuffer)` that mirrors `BuildLightmap` but reads `dFlags`/`dFlags::Lit`/explored instead of `dLight`. The visibility value per pixel is a `uint8_t` in `0..15` (0 = fully visible, 15 = pitch black).
5. `Lightmap::build` and `Lightmap::bleedUp` are extended to carry a parallel `visibilityMapBuffer`/`visibilityPitch`. `BlitPixelsWithLightmap` / `BlitPixelsBlendedWithLightmap` (`Source/engine/render/blit_impl.hpp:77, 157`) compute `effectiveLevel = max(lightLevel, visLevel)` before `adjustColor`. This means hidden tiles go black in both tile-LUT and per-pixel paths, and later Stages can give `visLevel` intermediate values for soft fade.
6. Stage 1 keeps `visLevel` binary (`0` or `15`); Stage 3 will compute intermediate values via marching squares (mirror of `BuildLightmap`'s quad interpolation at `Source/engine/render/light_render.cpp:431-495`).

## Failure Modes

- **Hellfire `DTYPE_NEST`/`DTYPE_CRYPT`** – `FullyDarkLightTable` is null (`Source/lighting.cpp:283`). The visibility channel avoids relying on it entirely: it feeds a `visLevel` straight into `max(light, vis)`. The shared channel works in all level types without per-level branching.
- **`DTYPE_HELL`** – `FullyLitLightTable` is null (`Source/lighting.cpp:276`); the red blood walls must keep their hue. Visibility is multiplicative in brightness sense (forces max level), so lit-but-blood-colored tiles still pass through `LightType::PartiallyLit` dispatch correctly.
- **Town (`DTYPE_TOWN`)** – all tiles `Lit` (`Source/diablo.cpp:3235-3241`); `IsTileVisibleToParty` always returns true, so the visibility channel is uniformly 0 and behaves as today.
- **Infravision / Arena** – existing `IsTileLit` overrides in `DrawPlayer`/`DrawMonsterHelper` (`Source/engine/render/scrollrt.cpp:468, 484`) are unchanged. Shadow culling only affects geometry and the per-pixel lightmap, not sprite gating.
- **Multiplayer coop** – `IsTileVisibleToParty` unions across active players via `VisionList`/`VisionActive` (`Source/lighting.h:38-41`). Matches `DungeonFlag::Explored`'s `MAP_EXP_OTHERS` semantics.
- **Save migration** – none; new option is forward-compatible because it has a default value.
- **Performance** – `BuildVisibilityMap` is half the cost of `BuildLightmap` (no falloff math, single byte per pixel). Combined with the dirty-`UpdateVision` flag (`Source/lighting.cpp:583`) the cache is reused across frames.

## Stages

Each stage is a separate, revertible commit and ships behind a setting default to `Off`.

### Stage 0 — Plumbing

- Add `enum class ShadowCullingMode : uint8_t { Off, Black, Memory };` in `Source/options.h` next to `FrameRateControl`.
- Register `OptionEntryEnum<ShadowCullingMode> shadowCulling` in `Source/options.h:550` and `Source/options.cpp:798` with translation strings `_("Shadow Culling")` and `_("Hide tiles outside the local party's line of sight.")`.
- Add `bool DebugShadowCulling = false;` to `Source/debug.cpp:35`, extern in `Source/debug.h:25`.
- Expose a Lua toggle in `Source/lua/modules/dev/display.cpp:27` mirroring `DebugVision`.
- No behavioral change.

### Stage 1 — Hard-black tile culling (target for first PR)

Goal: hidden tiles render pitch black, including in `perPixelLighting` mode.

1. New `Source/engine/render/visibility_render.{hpp,cpp}` with:
   - `bool IsTileVisibleToParty(Point p)` – loops `VisionActive[]`, calls existing `IsTileVisible`.
   - `uint8_t ComputeVisibilityLevel(Point p)` – returns `0` for visible/explored (per `ShadowCullingMode`), `LightsMax` for hidden.
   - `void BuildVisibilityMap(Point tilePosition, Point targetBufferPosition, uint16_t viewportWidth, uint16_t viewportHeight, int rows, int columns, std::span<uint8_t> visibilityMapBuffer)` – mirrors `BuildLightmap`'s quad walk at `Source/engine/render/light_render.cpp:431-495`, computing each tile's four corner vis values and rendering diamond via a `RenderCell` analogue. Reuses the same marching-squares helper to keep code in lock-step with the lightmap.
2. Extend `Lightmap` (`Source/engine/render/light_render.hpp`) to carry a `visibilityMapBuffer` / `visibilityPitch` parallel to `lightmapBuffer`. `getVisibilityAt(outLoc)` mirrors `getLightingAt`.
3. Modify `BlitPixelsWithLightmap` / `BlitFillWithLightmap` / `BlitPixelsBlendedWithLightmap` / `BlitFillBlendedWithLightmap` (`Source/engine/render/blit_impl.hpp:68-180`) to read both buffers and compute `effectiveLevel = max(lightLevel, visLevel)` before `lightmap.adjustColor(...)`. Same for the transparent variants used for arches (`scrollrt.cpp:922-925`).
4. Modify `Lightmap::build` to optionally build `visibilityMapBuffer` based on `shadowCulling`. When `Off`, visibility buffer is filled with 0 (identity, no cost in dispatch).
5. Modify `Lightmap::bleedUp` to mirror the operation on the visibility buffer (same offset arithmetic, same memcpy pattern at `light_render.cpp:557-575`).
6. Wall-bleed semantics: `Lightmap::bleedUp` already handles wall-light propagation. The visibility channel bleeds the same way, so a tall wall's pixels at the top will inherit the visibility of the floor below. This is what gives the canonical wall-silhouette look.
7. Replace the direct `dLight[...]` reads at `Source/engine/render/scrollrt.cpp:680` and `:782` with `ResolveEffectiveLight(tile, dLight[...] , visLevel)`. For Stage 1 the helper is trivial: `max(dLight, vis)`.

### Stage 2 — D2-style explored memory

Goal: tiles that have been seen but are not currently visible render as dark silhouettes (visLevel intermediate, e.g. 12).

1. Add a third mode value `Memory` to `ShadowCullingMode`.
2. `ComputeVisibilityLevel` returns `MemoryLevel` (e.g. 12) when `HasAnyOf(dFlags, DungeonFlag::Explored) && !IsTileVisibleToParty(p)`, `LightsMax` when never explored, `0` when visible. Slight tweak needed so `MemoryLevel` doesn't conflict with `LightsMax` in the dispatcher's `isFullyDarkLightTable` optimization – use an explicit `visibilityMask` bit that bypasses the LUT optimization entirely.
3. Add an `explored_silhouette.trn` (or reuse `pause.trn`) only if needed for diagnostics; not strictly required because the visibility channel handles the darkening.
4. Update `DebugShadowCulling` overlay to draw a color-coded square for each of the three states (visible / memory / hidden) using `pSquareCel` (mirror of `DebugVision` at `scrollrt.cpp:790`).

### Stage 3 — Soft per-pixel fade (Lord-of-Terror look)

Goal: pixels near LOS boundaries interpolate smoothly rather than going hard black.

1. In `BuildVisibilityMap`, instead of binary per-tile values, count how many of the 23 vision rays reach each tile (existing infrastructure: tally inside `DoVision`'s lambda at `lighting.cpp:206-213`). Pass that ray-hit count through `dFlags` (reuse the high bits of an unused flag, or add a transient `[MAXDUNX][MAXDUNY]` ray-count cache invalidated by `UpdateVision`).
2. Extend `BuildVisibilityMap`'s `RenderCell` to interpolate the per-tile ray count across each pixel using the same marching-squares pattern as `BuildLightmap`. Output range: 0..15, mapped so 0 rays = 15 and `radius*23` rays = 0.
3. Optional: distance falloff multiplier inside the visibility interpolation so far walls are darker than close ones.
4. No changes to blit ops needed beyond Stage 1's plumbing; this stage only changes the contents of the visibility buffer.

### Stage 4 — Sprite clipping (out of scope for first PR)

Goal: monsters/items straddling a LOS boundary don't visibly pop.

1. Approximate: alpha-blend the sprite against `LightType::PartiallyLit` with a `vis` table when straddling. Implement as a `ClxDrawWithVisibility` helper.
2. Pixel-perfect: precompute a per-tile visibility polygon as a CPU 1-bit sprite mask. Expensive; revisit only if Stage 4's approximate path proves insufficient.

## Validation

- **Compile** – `cmake --build build` on Linux SDL2 build, plus a SDL3 build (`-DDEVILUTIONX_SYSTEM_SDL3=ON`) to confirm the SDL3 branch at `Source/engine/dx.cpp:254-269` still works.
- **Unit tests** – extend `test/vision_test.cpp` (existing at `Source/levels/.../vision_test.cpp:96-180`) with a `shadowCulling=Black` scenario verifying `IsTileVisibleToParty` returns false behind a `BlockLight` wall.
- **Benchmark** – `test/light_render_benchmark.cpp` must remain within its prior range; the visibility pass adds at most one extra `memset` and one extra per-pixel `max()`.
- **Manual smoke**:
  - Load town – every tile visible regardless of mode.
  - Load Cathedral level 1, walk behind a wall with `Black` mode – tile behind the wall goes black; same tile becomes visible again when you step back into LOS.
  - Switch to `Memory` mode – tile behind a wall shows as a dark silhouette; previously-unvisited room is pitch black.
  - Toggle `perPixelLighting` and verify behavior is unchanged.
  - Hellfire: enter Crypt, confirm lava tiles are still warm (not full black) but tiles behind walls still occlude.
  - Coop: join a second player in town, walk into the dungeon, confirm visibility is the union of both players' vision (a tile behind a wall from player 1 but visible from player 2 still renders).
- **Debug overlay** – toggle `DebugShadowCulling`; squares color-coded by `visible/memory/hidden` overlay in the same way `DebugVision` already does.

## Open Questions

- Should `ShadowCullingMode` default to `Off` (safest, requires users to opt in) or `Black` (ships the headline feature by default)? Default is `Off` for first PR to keep behavior identical for existing users; revisit in a follow-up after community feedback.
- Should the new option live in `Graphics` (alongside `perPixelLighting`) or `Gameplay`? Plan currently puts it in `Graphics` because it is purely visual.

## Out of Scope

- True-color lighting (PR #559 path).
- New rendering backends, GPU shaders, or 32-bit textures.
- Network/save format changes.
- AI/automap/monster-AI behavior changes.
- Stage 4 sprite clipping (tracked as a follow-up).