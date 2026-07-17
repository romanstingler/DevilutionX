/**
 * @file gpu_backend.cpp
 *
 * Implementation of the SDL_GPU render backend declared in `gpu_backend.h`.
 *
 * Pipeline:
 *  - Engine writes 8-bit palette indices into `PalSurface` each frame.
 *  - We copy those pixels into an R8_UNORM source texture via a transfer buffer.
 *  - A palette lookup texture (256x1 RGBA8) is updated whenever the system
 *    palette changes.
 *  - A fullscreen vertex shader emits a triangle; the fragment shader reads
 *    the source texel, indexes the palette, and outputs RGBA.
 *
 * PR #1 reproduces the existing SDL_RenderTexture path 1:1. Custom upscale
 * shaders land in follow-up PRs by swapping the fragment shader / sampler
 * without touching this orchestration layer.
 */
#include "engine/render/gpu_sdl3/gpu_backend.h"

#include <cstdint>
#include <cstring>

#ifdef USE_SDL3
#include <SDL3/SDL_pixels.h>
#else
#include <SDL.h>
#endif

#include "engine/dx.h"
#include "engine/palette.h"
#include "headless_mode.hpp"
#include "options.h"
#include "utils/display.h"
#include "utils/log.hpp"

// Embedded shader blobs are produced at build time via glslangValidator +
// spirv_to_c.py. They live in gpu_shader_data.h next to this file.
#include "engine/render/gpu_sdl3/gpu_shader_data.h"

namespace devilution {

namespace {

SDL_GPUDevice *g_device = nullptr;
SDL_Window *g_window = nullptr;
bool g_swapchainClaimable = false;

SDL_GPUTexture *g_sourceTexture = nullptr;
SDL_GPUTexture *g_paletteTexture = nullptr;
SDL_GPUTextureFormat g_swapchainFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

SDL_GPUTransferBuffer *g_paletteTransfer = nullptr;
SDL_GPUTransferBuffer *g_sourceTransfer = nullptr;

SDL_GPUSampler *g_sampler = nullptr;

SDL_GPUShader *g_vertexShader = nullptr;
SDL_GPUShader *g_fragmentShader = nullptr;
SDL_GPUGraphicsPipeline *g_pipeline = nullptr;

uint32_t g_cachedScreenWidth = 0;
uint32_t g_cachedScreenHeight = 0;
ScalingQuality g_cachedScalingQuality = ScalingQuality::NearestPixel;

constexpr uint32_t kPaletteWidth = 256;
constexpr uint32_t kPaletteHeight = 1;

bool CreateDeviceAndClaim(SDL_Window *window)
{
#ifdef USE_SDL3
	if (!SDL_GPUSupportsShaderFormats(SDL_GPU_SHADERFORMAT_SPIRV, nullptr)) {
		LogError("SDL_GPU: no Vulkan/SPIRV support on this system; falling back to legacy renderer");
		return false;
	}
	g_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, /*debug_mode=*/false, /*name=*/"vulkan");
#else
	g_device = nullptr;
#endif
	if (g_device == nullptr) {
		LogError("SDL_GPU: SDL_CreateGPUDevice failed: {}", SDL_GetError());
		return false;
	}
	Log("SDL_GPU: created device using driver '{}'", SDL_GetGPUDeviceDriver(g_device));

#ifdef USE_SDL3
	if (!SDL_ClaimWindowForGPUDevice(g_device, window)) {
		LogError("SDL_GPU: SDL_ClaimWindowForGPUDevice failed: {}", SDL_GetError());
		SDL_DestroyGPUDevice(g_device);
		g_device = nullptr;
		return false;
	}
	g_swapchainClaimable = true;

	SDL_GPUSwapchainComposition composition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
	SDL_GPUPresentMode presentMode =
	    *GetOptions().Graphics.frameRateControl == FrameRateControl::VerticalSync
	    ? SDL_GPU_PRESENTMODE_VSYNC
	    : SDL_GPU_PRESENTMODE_MAILBOX;
	SDL_SetGPUSwapchainParameters(g_device, window, composition, presentMode);
#endif

	g_swapchainFormat = SDL_GetGPUSwapchainTextureFormat(g_device, window);
	g_window = window;
	return true;
}

void DestroyShadersAndPipeline()
{
	if (g_pipeline != nullptr) {
		SDL_ReleaseGPUGraphicsPipeline(g_device, g_pipeline);
		g_pipeline = nullptr;
	}
	if (g_fragmentShader != nullptr) {
		SDL_ReleaseGPUShader(g_device, g_fragmentShader);
		g_fragmentShader = nullptr;
	}
	if (g_vertexShader != nullptr) {
		SDL_ReleaseGPUShader(g_device, g_vertexShader);
		g_vertexShader = nullptr;
	}
}

bool CreateShadersAndPipeline()
{
#ifdef USE_SDL3
	SDL_GPUShaderCreateInfo vsInfo {};
	vsInfo.code = passthroughVertSpv;
	vsInfo.code_size = passthroughVertSpvLen;
	vsInfo.entrypoint = "main";
	vsInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
	vsInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
	vsInfo.num_uniform_buffers = 0;
	vsInfo.num_storage_buffers = 0;
	vsInfo.num_storage_textures = 0;
	vsInfo.num_samplers = 0;
	g_vertexShader = SDL_CreateGPUShader(g_device, &vsInfo);
	if (g_vertexShader == nullptr) {
		LogError("SDL_GPU: vertex shader creation failed: {}", SDL_GetError());
		return false;
	}

	SDL_GPUShaderCreateInfo fsInfo {};
	fsInfo.code = passthroughFragSpv;
	fsInfo.code_size = passthroughFragSpvLen;
	fsInfo.entrypoint = "main";
	fsInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
	fsInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
	fsInfo.num_samplers = 2;
	g_fragmentShader = SDL_CreateGPUShader(g_device, &fsInfo);
	if (g_fragmentShader == nullptr) {
		LogError("SDL_GPU: fragment shader creation failed: {}", SDL_GetError());
		DestroyShadersAndPipeline();
		return false;
	}

	SDL_GPUColorTargetDescription colorTarget {};
	colorTarget.format = g_swapchainFormat;
	colorTarget.blend_state = {};
	colorTarget.blend_state.enable_blend = false;
	colorTarget.blend_state.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;

	SDL_GPUGraphicsPipelineTargetInfo targetInfo {};
	targetInfo.num_color_targets = 1;
	targetInfo.color_target_descriptions = &colorTarget;
	targetInfo.has_depth_stencil_target = false;

	SDL_GPUGraphicsPipelineCreateInfo pipelineInfo {};
	pipelineInfo.vertex_shader = g_vertexShader;
	pipelineInfo.fragment_shader = g_fragmentShader;
	pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	pipelineInfo.rasterizer_state = {};
	pipelineInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
	pipelineInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
	pipelineInfo.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
	pipelineInfo.target_info = targetInfo;
	g_pipeline = SDL_CreateGPUGraphicsPipeline(g_device, &pipelineInfo);
	if (g_pipeline == nullptr) {
		LogError("SDL_GPU: graphics pipeline creation failed: {}", SDL_GetError());
		DestroyShadersAndPipeline();
		return false;
	}
	return true;
#else
	return false;
#endif
}

void DestroySampler()
{
	if (g_sampler != nullptr) {
		SDL_ReleaseGPUSampler(g_device, g_sampler);
		g_sampler = nullptr;
	}
}

bool CreateSampler()
{
#ifdef USE_SDL3
	const ScalingQuality quality = *GetOptions().Graphics.scaleQuality;
	SDL_GPUSamplerCreateInfo info {};
	info.min_filter = quality == ScalingQuality::NearestPixel ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
	info.mag_filter = info.min_filter;
	info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
	info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	if (quality == ScalingQuality::AnisotropicFiltering) {
		// Anisotropy support is requested via the SDL_PROP_GPU_DEVICE_CREATE_FEATURE_ANISOTROPY_BOOLEAN
		// property at device creation. If the driver does not support it, SDL silently disables
		// it; we still log the choice.
		info.enable_anisotropy = true;
		info.max_anisotropy = 16.0f;
	} else {
		info.enable_anisotropy = false;
		info.max_anisotropy = 1.0f;
	}
	g_sampler = SDL_CreateGPUSampler(g_device, &info);
	if (g_sampler == nullptr) {
		LogError("SDL_GPU: sampler creation failed: {}", SDL_GetError());
		return false;
	}
	g_cachedScalingQuality = quality;
	return true;
#else
	return false;
#endif
}

void DestroySourceResources()
{
	if (g_sourceTexture != nullptr) {
		SDL_ReleaseGPUTexture(g_device, g_sourceTexture);
		g_sourceTexture = nullptr;
	}
	if (g_sourceTransfer != nullptr) {
		SDL_ReleaseGPUTransferBuffer(g_device, g_sourceTransfer);
		g_sourceTransfer = nullptr;
	}
}

bool CreateSourceResources(uint32_t width, uint32_t height)
{
#ifdef USE_SDL3
	SDL_GPUTextureCreateInfo texInfo {};
	texInfo.type = SDL_GPU_TEXTURETYPE_2D;
	texInfo.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
	texInfo.width = width;
	texInfo.height = height;
	texInfo.layer_count_or_depth = 1;
	texInfo.num_levels = 1;
	texInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
	g_sourceTexture = SDL_CreateGPUTexture(g_device, &texInfo);
	if (g_sourceTexture == nullptr) {
		LogError("SDL_GPU: source texture creation failed: {}", SDL_GetError());
		return false;
	}

	const uint32_t transferSize = width * height;
	SDL_GPUTransferBufferCreateInfo tbInfo {};
	tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	tbInfo.size = transferSize;
	g_sourceTransfer = SDL_CreateGPUTransferBuffer(g_device, &tbInfo);
	if (g_sourceTransfer == nullptr) {
		LogError("SDL_GPU: source transfer buffer creation failed: {}", SDL_GetError());
		DestroySourceResources();
		return false;
	}

	g_cachedScreenWidth = width;
	g_cachedScreenHeight = height;
	return true;
#else
	return false;
#endif
}

void DestroyPaletteResources()
{
	if (g_paletteTexture != nullptr) {
		SDL_ReleaseGPUTexture(g_device, g_paletteTexture);
		g_paletteTexture = nullptr;
	}
	if (g_paletteTransfer != nullptr) {
		SDL_ReleaseGPUTransferBuffer(g_device, g_paletteTransfer);
		g_paletteTransfer = nullptr;
	}
}

bool CreatePaletteResources()
{
#ifdef USE_SDL3
	SDL_GPUTextureCreateInfo texInfo {};
	texInfo.type = SDL_GPU_TEXTURETYPE_2D;
	texInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	texInfo.width = kPaletteWidth;
	texInfo.height = kPaletteHeight;
	texInfo.layer_count_or_depth = 1;
	texInfo.num_levels = 1;
	texInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
	g_paletteTexture = SDL_CreateGPUTexture(g_device, &texInfo);
	if (g_paletteTexture == nullptr) {
		LogError("SDL_GPU: palette texture creation failed: {}", SDL_GetError());
		return false;
	}

	SDL_GPUTransferBufferCreateInfo tbInfo {};
	tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	tbInfo.size = kPaletteWidth * 4;
	g_paletteTransfer = SDL_CreateGPUTransferBuffer(g_device, &tbInfo);
	if (g_paletteTransfer == nullptr) {
		LogError("SDL_GPU: palette transfer buffer creation failed: {}", SDL_GetError());
		DestroyPaletteResources();
		return false;
	}
	return true;
#else
	return false;
#endif
}

bool IsScreenSizeCurrent()
{
	return g_cachedScreenWidth == static_cast<uint32_t>(gnScreenWidth)
	    && g_cachedScreenHeight == static_cast<uint32_t>(gnScreenHeight);
}

bool EnsureSourceSizeMatchesScreen()
{
	if (g_sourceTexture == nullptr || !IsScreenSizeCurrent()) {
		DestroySourceResources();
		if (!CreateSourceResources(static_cast<uint32_t>(gnScreenWidth),
		        static_cast<uint32_t>(gnScreenHeight))) {
			return false;
		}
	}
	return true;
}

bool EnsureSamplerMatchesQuality()
{
	if (g_sampler != nullptr && g_cachedScalingQuality == *GetOptions().Graphics.scaleQuality) {
		return true;
	}
	DestroySampler();
	return CreateSampler();
}

} // namespace

void GPUBackendInit(SDL_Window *window)
{
	if (HeadlessMode) return;
	if (!*GetOptions().Graphics.gpuBackend) return;
	if (g_device != nullptr) return;

	if (!CreateDeviceAndClaim(window)) return;
	if (!CreateSourceResources(static_cast<uint32_t>(gnScreenWidth),
	        static_cast<uint32_t>(gnScreenHeight))) {
		GPUBackendShutdown();
		return;
	}
	if (!CreatePaletteResources()) {
		GPUBackendShutdown();
		return;
	}
	if (!CreateSampler()) {
		GPUBackendShutdown();
		return;
	}
	if (!CreateShadersAndPipeline()) {
		GPUBackendShutdown();
		return;
	}

	// Push an initial palette so the very first frame is correct.
	GPUBackendOnPaletteChanged();
	Log("SDL_GPU backend initialized ({}x{} source, swapchain format {})",
	    g_cachedScreenWidth, g_cachedScreenHeight, static_cast<int>(g_swapchainFormat));
}

void GPUBackendShutdown()
{
	if (g_device == nullptr) return;

#ifdef USE_SDL3
	if (g_pipeline != nullptr) {
		SDL_ReleaseGPUGraphicsPipeline(g_device, g_pipeline);
		g_pipeline = nullptr;
	}
	if (g_fragmentShader != nullptr) {
		SDL_ReleaseGPUShader(g_device, g_fragmentShader);
		g_fragmentShader = nullptr;
	}
	if (g_vertexShader != nullptr) {
		SDL_ReleaseGPUShader(g_device, g_vertexShader);
		g_vertexShader = nullptr;
	}
	if (g_sampler != nullptr) {
		SDL_ReleaseGPUSampler(g_device, g_sampler);
		g_sampler = nullptr;
	}
	DestroySourceResources();
	DestroyPaletteResources();
	if (g_swapchainClaimable) {
		SDL_ReleaseWindowFromGPUDevice(g_device, g_window);
		g_swapchainClaimable = false;
	}
	SDL_DestroyGPUDevice(g_device);
#endif
	g_device = nullptr;
	g_window = nullptr;
}

bool GPUBackendIsAvailable()
{
	return g_device != nullptr;
}

bool GPUBackendRenderFrame()
{
#ifdef USE_SDL3
	if (g_device == nullptr || g_window == nullptr) return false;
	if (HeadlessMode) return false;
	if (!EnsureSourceSizeMatchesScreen()) return false;
	if (!EnsureSamplerMatchesQuality()) return false;

	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_device);
	if (cmd == nullptr) {
		LogError("SDL_GPU: SDL_AcquireGPUCommandBuffer failed: {}", SDL_GetError());
		return false;
	}

	SDL_GPUTexture *swapchain = nullptr;
	Uint32 swapchainWidth = 0;
	Uint32 swapchainHeight = 0;
	if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, g_window, &swapchain, &swapchainWidth, &swapchainHeight)) {
		LogError("SDL_GPU: acquire swapchain failed: {}", SDL_GetError());
		SDL_CancelGPUCommandBuffer(cmd);
		return false;
	}
	if (swapchain == nullptr) {
		SDL_SubmitGPUCommandBuffer(cmd);
		return true; // window minimized; nothing to draw
	}

	// Upload the latest PalSurface contents to the source texture.
	{
		SDL_GPUTextureTransferInfo src {};
		src.transfer_buffer = g_sourceTransfer;
		src.offset = 0;

		SDL_GPUTextureRegion dst {};
		dst.texture = g_sourceTexture;
		dst.w = g_cachedScreenWidth;
		dst.h = g_cachedScreenHeight;
		dst.d = 1;

		SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(cmd);
		void *mapped = SDL_MapGPUTransferBuffer(g_device, g_sourceTransfer, /*cycle=*/false);
		if (mapped != nullptr && PalSurface != nullptr && PalSurface->pixels != nullptr) {
			const uint32_t srcPitch = static_cast<uint32_t>(PalSurface->pitch);
			const uint32_t copyBytesPerRow = g_cachedScreenWidth; // R8_UNORM = 1 byte/texel
			if (srcPitch == copyBytesPerRow) {
				std::memcpy(mapped, PalSurface->pixels, copyBytesPerRow * g_cachedScreenHeight);
			} else {
				auto *dstBytes = static_cast<uint8_t *>(mapped);
				const auto *srcBytes = static_cast<uint8_t *>(PalSurface->pixels);
				for (uint32_t y = 0; y < g_cachedScreenHeight; ++y) {
					std::memcpy(dstBytes + y * copyBytesPerRow,
					    srcBytes + y * srcPitch,
					    copyBytesPerRow);
				}
			}
		}
		SDL_UnmapGPUTransferBuffer(g_device, g_sourceTransfer);

		SDL_UploadToGPUTexture(copyPass, &src, &dst, /*cycle=*/false);
		SDL_EndGPUCopyPass(copyPass);
	}

	// Render pass: bind pipeline and source + palette textures, draw a fullscreen triangle.
	{
		SDL_GPUColorTargetInfo target {};
		target.texture = swapchain;
		target.clear_color = { 0.0f, 0.0f, 0.0f, 1.0f };
		target.load_op = SDL_GPU_LOADOP_CLEAR;
		target.store_op = SDL_GPU_STOREOP_STORE;
		target.cycle = false;

		SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
		SDL_BindGPUGraphicsPipeline(pass, g_pipeline);

		SDL_GPUTextureSamplerBinding sourceBinding {};
		sourceBinding.texture = g_sourceTexture;
		sourceBinding.sampler = g_sampler;
		SDL_BindGPUFragmentSamplers(pass, 0, &sourceBinding, 1);

		SDL_GPUTextureSamplerBinding paletteBinding {};
		paletteBinding.texture = g_paletteTexture;
		paletteBinding.sampler = g_sampler;
		SDL_BindGPUFragmentSamplers(pass, 1, &paletteBinding, 1);

		SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
		SDL_EndGPURenderPass(pass);
	}

	SDL_SubmitGPUCommandBuffer(cmd);
	return true;
#else
	return false;
#endif
}

void GPUBackendOnPaletteChanged()
{
#ifdef USE_SDL3
	if (g_device == nullptr || g_paletteTransfer == nullptr || g_paletteTexture == nullptr) return;

	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_device);
	if (cmd == nullptr) return;

	void *mapped = SDL_MapGPUTransferBuffer(g_device, g_paletteTransfer, /*cycle=*/false);
	if (mapped != nullptr) {
		auto *dst = static_cast<uint8_t *>(mapped);
		for (uint32_t i = 0; i < kPaletteWidth; ++i) {
			const SDL_Color &c = system_palette[i];
			dst[i * 4 + 0] = c.r;
			dst[i * 4 + 1] = c.g;
			dst[i * 4 + 2] = c.b;
			dst[i * 4 + 3] = SDL_ALPHA_OPAQUE;
		}
	}
	SDL_UnmapGPUTransferBuffer(g_device, g_paletteTransfer);

	SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUTextureTransferInfo src {};
	src.transfer_buffer = g_paletteTransfer;
	src.offset = 0;
	SDL_GPUTextureRegion dst {};
	dst.texture = g_paletteTexture;
	dst.w = kPaletteWidth;
	dst.h = kPaletteHeight;
	dst.d = 1;
	SDL_UploadToGPUTexture(copyPass, &src, &dst, /*cycle=*/false);
	SDL_EndGPUCopyPass(copyPass);

	SDL_SubmitGPUCommandBuffer(cmd);
#endif
}

} // namespace devilution