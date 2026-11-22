/**
 * Sleeping Dogs port (SDNX): native renderer backend, skeleton (docs/native-renderer-plan.md in the port).
 *
 * The SDK's CommandProcessor keeps reading the PM4 ring (packets, registers, waits, interrupts, events); only
 * the expensive parts of the Xenos emulation are replaced: draws, resolves, swap, shader loads. No write-watched
 * shared memory, no run-time shader translation. Selected by the cvar sdnx_native_renderer or SDNX_NATIVE=1.
 */
#pragma once

#include <memory>
#include <string>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>

// The cvars are defined at global scope (REXCVAR_DEFINE_BOOL in the .cpp), so the declaration is too.
// Switch: record through deko3d instead of Vulkan/NVK (sdretail.toml: sdnx_deko3d_backend = true).
REXCVAR_DECLARE(bool, sdnx_deko3d_backend);
// Diagnostics for the deko3d bring-up (sdretail.toml): skip draws, skip texture binding, skip the front-to-
// swapchain copy (present a flat colour instead). Each isolates a stage that a GPU fault could come from.
REXCVAR_DECLARE(bool, sdnx_deko3d_no_draw);
REXCVAR_DECLARE(bool, sdnx_deko3d_no_tex);
REXCVAR_DECLARE(bool, sdnx_deko3d_no_blit);
// Diagnostic: clear colours become magenta, every draw writes all channels with blending off. If the screen turns
// magenta the pipeline and the output path work and the draws themselves write black; if it stays black the loss is
// in the resolve/present path.
REXCVAR_DECLARE(bool, sdnx_deko3d_magenta);
// Diagnostic: magenta only in the pass that writes the screen-sized target. Magenta there means the final pass
// works and its input textures are empty; staying black means the final pass itself is broken.
REXCVAR_DECLARE(bool, sdnx_deko3d_magenta_final);
// Diagnostic: no polygon culling at all. After the vertical flip the winding of every triangle is reversed, so a
// draw that relied on culling can drop all of its triangles; with culling off they must appear if that is the cause.
REXCVAR_DECLARE(bool, sdnx_deko3d_no_cull);
// SDNX §16.116: the vertex shaders' own near/far clipping (dk_build.py CLIP_WRAPPER), off unless set in the toml.
REXCVAR_DECLARE(bool, sdnx_deko3d_clip);
// SDNX §16.117: deko3d frame logs (swaps 900, 1800) with the resolve/stencil probes and the image dumps.
REXCVAR_DECLARE(bool, sdnx_deko3d_frame_log);
// SDNX §16.118: the per-draw breadcrumb report of §16.94 (for GPU faults), off by default.
REXCVAR_DECLARE(bool, sdnx_deko3d_breadcrumb);
// SDNX §16.123: the deko3d backend's internal render scale (percent; 100, 75, 50).
REXCVAR_DECLARE(int32_t, sdnx_render_scale);
REXCVAR_DECLARE(bool, sdnx_early_z);
REXCVAR_DECLARE(bool, sdnx_depth_only_vs);
REXCVAR_DECLARE(bool, sdnx_fxaa_zero_signs);
REXCVAR_DECLARE(bool, sdnx_gpu_diagnostics);
REXCVAR_DECLARE(bool, sdnx_zcull);
REXCVAR_DECLARE(bool, sdnx_push_constants);
REXCVAR_DECLARE(bool, sdnx_upload_cached);
REXCVAR_DECLARE(bool, sdnx_hash_single_lookup);  // §16.162
REXCVAR_DECLARE(bool, sdnx_deko3d_queries);      // §16.163
REXCVAR_DECLARE(bool, sdnx_depth_direct);        // §16.165
REXCVAR_DECLARE(bool, sdnx_hash_inline);         // §16.166
REXCVAR_DECLARE(bool, sdnx_texmemo_keep);        // §16.168
REXCVAR_DECLARE(bool, sdnx_upload_zva);          // §16.169
REXCVAR_DECLARE(bool, sdnx_shared_lean);         // §16.169
REXCVAR_DECLARE(bool, sdnx_capture_touch);       // §16.170
REXCVAR_DECLARE(bool, sdnx_prefetch_shaders);    // §16.171
REXCVAR_DECLARE(bool, sdnx_tile_merge);          // §16.172
REXCVAR_DECLARE(bool, sdnx_sparse_constants);    // §16.181
REXCVAR_DECLARE(bool, sdnx_texmemo_frames);      // §16.182
REXCVAR_DECLARE(bool, sdnx_hash_stable_long);    // §16.182
REXCVAR_DECLARE(std::string, sdnx_skip_ps);      // §16.173
REXCVAR_DECLARE(std::string, sdnx_skip_cycle);   // §16.174
REXCVAR_DECLARE(bool, sdnx_hash_stable_skip);    // §16.162
REXCVAR_DECLARE(int32_t, sdnx_sharpen);
REXCVAR_DECLARE(int32_t, sdnx_submits_in_flight);

namespace rex::graphics::sdnx {

// True when the native renderer is requested (cvar sdnx_native_renderer or environment SDNX_NATIVE=1; the
// cvar may not reach a plugin DLL on the desktop, the environment always does).
bool NativeRendererRequested();


class NativeGraphicsSystem : public GraphicsSystem {
 public:
  NativeGraphicsSystem();
  ~NativeGraphicsSystem() override;

  std::string name() const override;

  // deko3d on the Switch: the backend owns the NWindow swapchain, so there is no SDK provider or presenter and
  // the base class's "Unable to create graphics provider" failure is skipped (portlog 16.58).
  X_STATUS SetupPresentation(::rex::ui::WindowedAppContext* app_context) override;

 protected:
  void CreateProvider(bool with_presentation) override;

 private:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::graphics::sdnx






