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
REXCVAR_DEFINE_BOOL(sdnx_push_constants, false, "GPU",
                    "deko3d: shader constants in persistent blocks updated in the command stream with only the changed "
                    "registers, instead of a copy of the whole block per draw (SDNX 16.139; off since 16.140: slower, the game rewrites nearly every constant before each draw)");
REXCVAR_DEFINE_BOOL(sdnx_zcull, true, "GPU",
                    "deko3d: hardware ZCULL (coarse rejection of hidden pixels before shading, also for shaders with "
                    "kill), invalidated after every depth/stencil write outside the 3D engine (SDNX 16.131)");
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







