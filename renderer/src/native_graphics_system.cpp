/**
 * Sleeping Dogs port (SDNX): native renderer backend, graphics system.
 */
#include <cstdlib>

#include <rex/cvar.h>
#include <rex/graphics/sdnx/native_command_processor.h>
#include <rex/graphics/sdnx/native_graphics_system.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/vulkan/provider.h>

REXCVAR_DEFINE_BOOL(sdnx_deko3d_backend, false, "GPU",
                    "Switch: render through deko3d instead of Vulkan/NVK (needs its presenter; also SDNX_DEKO3D=1)");
REXCVAR_DEFINE_BOOL(sdnx_deko3d_no_draw, false, "GPU", "deko3d diagnostic: skip every draw");
REXCVAR_DEFINE_BOOL(sdnx_deko3d_no_tex, false, "GPU", "deko3d diagnostic: bind no textures");
REXCVAR_DEFINE_BOOL(sdnx_deko3d_no_blit, false, "GPU",
                    "deko3d diagnostic: no front-to-swapchain copy (flat colour)");
REXCVAR_DEFINE_BOOL(sdnx_deko3d_magenta, false, "GPU", "deko3d diagnostic: magenta clears, full colour mask, no blending");
REXCVAR_DEFINE_BOOL(sdnx_deko3d_magenta_final, false, "GPU",
                    "deko3d diagnostic: magenta only in the screen-sized (final) pass");
REXCVAR_DEFINE_BOOL(sdnx_deko3d_no_cull, false, "GPU", "deko3d diagnostic: culling off (winding is reversed by the Y flip)");
REXCVAR_DEFINE_INT32(sdnx_render_scale, 100, "GPU",
                     "deko3d internal render scale in percent of the guest resolution: 100, 75 or 50 (SDNX 16.123)");
REXCVAR_DEFINE_INT32(sdnx_submits_in_flight, 4, "GPU",
                     "deko3d: submits the GPU may still have queued when the CPU sends another (2-8); more lets the CPU "
                     "run further ahead, too many overflowed the channel's gpfifo in the past (SDNX 16.133)");
REXCVAR_DEFINE_INT32(sdnx_sharpen, -1, "GPU",
                     "deko3d: contrast-adaptive sharpening of the frame on its way to the screen, 0-100 (0 = off, a plain "
                     "blit); -1 = by render scale: 100 % off, 75 % 35, 50 % 60 (SDNX 16.148)");
REXCVAR_DEFINE_BOOL(sdnx_upload_cached, true, "GPU",
                    "deko3d: the upload ring (constants, texture data, vertices) in CPU-cached memory, cleaned from the "
                    "cache before each submit, instead of write-combined memory (SDNX 16.141)");
REXCVAR_DEFINE_STRING(sdnx_skip_cycle,
                      "ps_75b785e4bef74504+ps_815eea133da1cdfd,ps_a635fb6ecdee8391,ps_a7f9a6856e5ae036,ps_c9a5881ccfb7202e",
                      "GPU", "Four-finger touch cycles through these groups (comma) of pixel shaders (plus) not drawn "
                      "(SDNX 16.174)");
REXCVAR_DEFINE_STRING(sdnx_skip_ps, "", "GPU",
                      "Comma-separated parts of pixel shader names whose draws are not drawn (SDNX 16.173)");
REXCVAR_DEFINE_BOOL(sdnx_hash_stable_long, true, "GPU",
                    "Geometry cache: content unchanged for 600+ frames is checked every 8th frame (SDNX 16.182)");
REXCVAR_DEFINE_BOOL(sdnx_texmemo_frames, true, "GPU",
                    "Texture descriptor memo kept from frame to frame for ordinary textures (SDNX 16.182)");
REXCVAR_DEFINE_BOOL(sdnx_sparse_constants, true, "GPU",
                    "Shader constants: only the registers a shader reads are copied per draw (SDNX 16.181)");
REXCVAR_DEFINE_BOOL(sdnx_tile_merge, true, "GPU",
                    "The scene pass the Xbox 360 draws in two tiles is drawn once at full height (SDNX 16.172)");
REXCVAR_DEFINE_BOOL(sdnx_prefetch_shaders, true, "GPU",
                    "deko3d: the shader objects of a draw are prefetched at its start (SDNX 16.171)");
REXCVAR_DEFINE_BOOL(sdnx_capture_touch, true, "GPU",
                    "A three-finger touch logs the next frame and dumps its picture (SDNX 16.170)");
REXCVAR_DEFINE_BOOL(sdnx_upload_zva, true, "GPU",
                    "Constant uploads allocate cache lines with DC ZVA instead of reading them first (SDNX 16.169)");
REXCVAR_DEFINE_BOOL(sdnx_shared_lean, true, "GPU",
                    "Shared constant block kept between draws: only descriptor numbers cleared, pull info copied only when used (SDNX 16.169)");
REXCVAR_DEFINE_BOOL(sdnx_texmemo_keep, true, "GPU",
                    "Texture descriptor memo kept over resolves for ordinary textures (SDNX 16.168)");
REXCVAR_DEFINE_BOOL(sdnx_hash_inline, true, "GPU",
                    "Geometry hash table with the values inside the buckets: one cache miss per lookup instead of two (SDNX 16.166)");
REXCVAR_DEFINE_BOOL(sdnx_depth_direct, true, "GPU",
                    "deko3d: depth resolves copy the extracted depth and stencil straight into their images instead of through a buffer (SDNX 16.165)");
REXCVAR_DEFINE_BOOL(sdnx_deko3d_queries, true, "GPU",
                    "deko3d: occlusion queries counted by the GPU (samples passed). false = every query is answered as visible, as before 16.163 (sun flares and light coronas then show through walls)");
REXCVAR_DEFINE_BOOL(sdnx_hash_single_lookup, true, "GPU",
                    "Geometry cache: the entry found by the reuse check is filled directly, without a second table lookup (SDNX 16.162)");
REXCVAR_DEFINE_BOOL(sdnx_hash_stable_skip, true, "GPU",
                    "Geometry cache: ranges unchanged for 120+ frames are hashed every 4th frame instead of every frame (SDNX 16.162). Off if geometry flickers or shows stale shapes for a moment");
REXCVAR_DEFINE_BOOL(sdnx_push_constants, false, "GPU",
                    "deko3d: shader constants in persistent blocks updated in the command stream with only the changed "
                    "registers, instead of a copy of the whole block per draw (SDNX 16.139; off since 16.140: slower, the game rewrites nearly every constant before each draw)");
REXCVAR_DEFINE_BOOL(sdnx_zcull, true, "GPU",
                    "deko3d: hardware ZCULL (coarse rejection of hidden pixels before shading, also for shaders with "
                    "kill), invalidated after every depth/stencil write outside the 3D engine (SDNX 16.131)");
REXCVAR_DEFINE_BOOL(sdnx_gpu_diagnostics, false, "GPU",
                    "deko3d: enable periodic synchronous GPU profiling and scene probes");
REXCVAR_DEFINE_BOOL(sdnx_fxaa_zero_signs, true, "GPU",
                    "deko3d: specialize FXAA when both texture sign masks are zero");
REXCVAR_DEFINE_BOOL(sdnx_depth_only_vs, true, "GPU",
                    "deko3d: use the position-only vertex variant for the 5add depth pass");
REXCVAR_DEFINE_BOOL(sdnx_early_z, true, "GPU",
                    "deko3d: bind the pixel shader variant without the alpha-test discard while the alpha test is off, "
                    "so hidden pixels are rejected before shading (SDNX 16.129)");
REXCVAR_DEFINE_BOOL(sdnx_deko3d_breadcrumb, false, "GPU",
                    "deko3d diagnostic (SDNX 16.94/16.118): report each finished draw's number (costs GPU time)");
REXCVAR_DEFINE_BOOL(sdnx_deko3d_frame_log, false, "GPU",
                    "deko3d diagnostic (SDNX 16.117): frame logs of swaps 900/1800 with resolve probes and image dumps");
REXCVAR_DEFINE_BOOL(sdnx_deko3d_clip, false, "GPU",
                    "deko3d test (SDNX 16.116): Direct3D near/far clipping in the vertex shaders (gl_ClipDistance)");
REXCVAR_DEFINE_BOOL(sdnx_native_renderer, false, "GPU",
                    "Sleeping Dogs native renderer instead of the Xenos emulation (also SDNX_NATIVE=1)");

namespace rex::graphics::sdnx {

bool NativeRendererRequested() {
  const char* env = std::getenv("SDNX_NATIVE");
  if (env && *env && *env != '0') {
    return true;
  }
  // One switch is enough on the console: asking for the deko3d backend also asks for the native renderer (without
  // it the game runs the stock Xenos emulation, which never looks at the backend cvar).
  const char* deko3d_env = std::getenv("SDNX_DEKO3D");
  if (deko3d_env && *deko3d_env && *deko3d_env != '0') {
    return true;
  }
  return REXCVAR_GET(sdnx_native_renderer) || REXCVAR_GET(sdnx_deko3d_backend);
}

NativeGraphicsSystem::NativeGraphicsSystem() {}
NativeGraphicsSystem::~NativeGraphicsSystem() {}

static bool Deko3dRequested() {
  const char* env = std::getenv("SDNX_DEKO3D");
  if (env && *env && *env != '0') return true;
  return REXCVAR_GET(sdnx_deko3d_backend);
}

X_STATUS NativeGraphicsSystem::SetupPresentation(::rex::ui::WindowedAppContext* app_context) {
#if REX_PLATFORM_SWITCH
  if (Deko3dRequested()) {
    // The deko3d backend owns the swapchain (NWindow) and presents the frames itself: the base class would ask
    // for a graphics provider and fail ("Unable to create graphics provider", X_STATUS_UNSUCCESSFUL).
    REXGPU_INFO("[sdnx_native] deko3d: no SDK provider or presenter, the backend presents");
    return X_STATUS_SUCCESS;
  }
#endif
  return GraphicsSystem::SetupPresentation(app_context);
}

std::string NativeGraphicsSystem::name() const {
  return "SDNX native (Vulkan)";
}

void NativeGraphicsSystem::CreateProvider(bool with_presentation) {
#if REX_PLATFORM_SWITCH
  // deko3d mode: no Vulkan at all on the console. The backend owns the swapchain (NWindow) and presents the
  // frames itself, so the graphics system runs without a provider and without the SDK presenter.
  const char* env = std::getenv("SDNX_DEKO3D");
  if ((env && *env && *env != '0') || REXCVAR_GET(sdnx_deko3d_backend)) {
    REXGPU_INFO("[sdnx_native] deko3d mode: no Vulkan provider, the backend presents its own swapchain");
    return;
  }
#endif
  // The XenosRecomp SPIR-V needs device features requested at creation (bindless arrays, int64, ...).
  rex::cvar::SetFlagByName("vulkan_native_shader_features", "true");
  provider_ = rex::ui::vulkan::VulkanProvider::Create(true, with_presentation);
}

std::unique_ptr<CommandProcessor> NativeGraphicsSystem::CreateCommandProcessor() {
  REXGPU_INFO("[sdnx_native] native renderer: no Xenos draw emulation (stage 2 skeleton)");
  return std::make_unique<NativeCommandProcessor>(this, kernel_state_);
}

}  // namespace rex::graphics::sdnx







