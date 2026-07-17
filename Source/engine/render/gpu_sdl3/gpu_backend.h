/**
 * @file gpu_backend.h
 *
 * SDL_GPU render backend for DevilutionX.
 *
 * This module owns an `SDL_GPUDevice`, an offscreen source texture for the
 * engine's 8-bit indexed `PalSurface`, a 256-color palette lookup texture, and
 * a passthrough graphics pipeline. Per frame the engine's pixel buffer is
 * uploaded to the source texture and a fullscreen draw composites it through
 * the palette to the window's swapchain texture.
 *
 * The backend is opt-in (gated on `USE_SDL3_GPU` at build time, `gpuBackend`
 * at runtime) and falls back to the legacy SDL_RenderTexture path when the
 * GPU device cannot be created or no supported shader format is found.
 */
#pragma once

#include <cstdint>

#ifdef USE_SDL3
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_video.h>
#else
#include <SDL.h>
#endif

namespace devilution {

/**
 * @brief Tries to create the GPU backend bound to `window`.
 *
 * On success, `IsAvailable()` returns true and `RenderFrame()` / `Shutdown()`
 * become valid. On failure (no supported driver, no SPIRV support, etc.) the
 * backend stays inactive and the engine falls back to the SDL_RenderTexture
 * path. The failure reason is logged.
 *
 * Safe to call when `*GetOptions().Graphics.gpuBackend` is false: it returns
 * without doing anything.
 */
void GPUBackendInit(SDL_Window *window);

/** @brief Tears down all GPU resources and detaches from the window. */
void GPUBackendShutdown();

/**
 * @brief Uploads the engine's `PalSurface` and draws a frame to the window.
 *
 * Returns false when the backend is inactive or the swapchain texture could
 * not be acquired this frame (so the caller can retry with the legacy path).
 */
bool GPUBackendRenderFrame();

/**
 * @brief Updates the palette lookup texture from the current system palette.
 *
 * Called from the palette update path so palette fades / color cycling show up
 * on the GPU-composited output without going through the legacy renderer.
 */
void GPUBackendOnPaletteChanged();

/** @brief True iff the backend is currently active and `RenderFrame` is usable. */
bool GPUBackendIsAvailable();

} // namespace devilution