/**
 * Sleeping Dogs port (SDNX): native renderer command processor (stage 2b).
 *
 * The base CommandProcessor parses the PM4 ring. Here:
 *  - EDRAM-lite: host images per render target configuration (no format aliasing yet);
 *  - resolves (IssueCopy): blit of the resolve rectangle into a host "resolved texture" keyed by the physical
 *    destination address, then the clears the resolve requests;
 *  - swap: the front buffer (texture fetch constant 0, as Direct3D sets it) is copied by a compute shader into
 *    the presenter's guest output image;
 *  - draws: counted only (next step);
 *  - statistics every 5 s.
 * All work of a frame goes into one command buffer, submitted at the swap.
 */
#include <rex/graphics/sdnx/native_command_processor.h>
#include <rex/graphics/sdnx/native_graphics_system.h>  // REXCVAR_DECLARE(sdnx_deko3d_backend)

#if REX_PLATFORM_SWITCH
#include "deko3d_backend.h"
#endif

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <sstream>
#include <string>

#include <rex/graphics/graphics_system.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/string/buffer.h>
#if REX_PLATFORM_SWITCH
#include <switch.h>  // §16.170: the touch screen (three-finger capture)
#endif


namespace rex::graphics::sdnx {

namespace {

VkFormat ColorTargetFormat(xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    default:  // 7e3, 16_16_16_16 (fixed and float)
      return VK_FORMAT_R16G16B16A16_SFLOAT;
  }
}

VkFormat ResolveDestFormat(xenos::TextureFormat format, VkFormat source) {
  switch (format) {
    case xenos::TextureFormat::k_8_8_8_8:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case xenos::TextureFormat::k_2_10_10_10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case xenos::TextureFormat::k_16_16_16_16:
      return VK_FORMAT_R16G16B16A16_UNORM;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case xenos::TextureFormat::k_16_16:
      return VK_FORMAT_R16G16_UNORM;
    case xenos::TextureFormat::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case xenos::TextureFormat::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case xenos::TextureFormat::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    case xenos::TextureFormat::k_8:
      return VK_FORMAT_R8_UNORM;
    case xenos::TextureFormat::k_8_8:
      return VK_FORMAT_R8G8_UNORM;
    default:
      return source;
  }
}

// RB_COLOR_CLEAR is the packed value in the render target format.
VkClearColorValue DecodeColorClear(xenos::ColorRenderTargetFormat format, uint32_t lo, uint32_t hi) {
  VkClearColorValue c = {};
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      for (int i = 0; i < 4; ++i) c.float32[i] = float((lo >> (8 * i)) & 0xFF) / 255.0f;
      break;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      for (int i = 0; i < 3; ++i) c.float32[i] = float((lo >> (10 * i)) & 0x3FF) / 1023.0f;
      c.float32[3] = float(lo >> 30) / 3.0f;
      break;
    default:
      // Float formats: zero is by far the most common clear; exact decoding comes with EDRAM aliasing.
      (void)hi;
      break;
  }
  return c;
}

}  // namespace

void NativeCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  if (index >= 0x2000 && index < RegisterFile::kRegisterCount) {
    register_file_->values[index] = value;
    if (index >= 0x4000 && index < 0x4800) NoteConstantWrites(index, 1);  // §16.135
    return;
  }
  CommandProcessor::WriteRegister(index, value);
}

void NativeCommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers) {
  if (start_index >= 0x2000 && start_index + num_registers <= RegisterFile::kRegisterCount) {
    uint32_t* dst = &register_file_->values[start_index];
    for (uint32_t i = 0; i < num_registers; ++i) dst[i] = __builtin_bswap32(base[i]);
    NoteConstantWrites(start_index, num_registers);  // §16.135
    return;
  }
  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
}

NativeCommandProcessor::NativeCommandProcessor(GraphicsSystem* graphics_system,
                                               system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}

NativeCommandProcessor::~NativeCommandProcessor() = default;

uint64_t NativeCommandProcessor::RenderTargetKey(uint32_t base_tiles, uint32_t pitch_pixels, uint32_t format,
                                                 xenos::MsaaSamples msaa, bool is_depth) {
  // Key by the *host* format and the depth flag, not by the guest enum: the guest has aliases that map to
  // one host format (k_8_8_8_8 and k_8_8_8_8_GAMMA both to R8G8B8A8, 7e3 and 16_16_16_16 both to
  // R16G16B16A16_SFLOAT). Keying by the guest value made the draws and the resolve use two separate images
  // with the same address, so the resolve read an empty one and the frame stayed black.
  uint32_t host_format = is_depth ? 0x3Fu : uint32_t(ColorTargetFormat(xenos::ColorRenderTargetFormat(format)));
  // Fields: base 12 bits, pitch 16 bits, host format 8 bits (VkFormat values of the color formats used are
  // < 256), MSAA 2 bits, depth 1 bit - the format used to spill into the MSAA bits.
  return uint64_t(base_tiles & 0xFFF) | (uint64_t(pitch_pixels & 0xFFFF) << 12) |
         (uint64_t(host_format & 0xFF) << 28) | (uint64_t(msaa) << 36) | (uint64_t(is_depth) << 38);
}

bool NativeCommandProcessor::CreateImage(Image& image, VkFormat format, uint32_t width, uint32_t height,
                                         bool depth) {
  static const bool trace_image = std::getenv("SDNX_TRACE_DRAW") != nullptr;
  if (trace_image) {
    REXGPU_INFO("[sdnx_trace] create image {}x{} format {} depth={}", width, height, int(format), depth);
  }
  ImageDesc desc;
  desc.format = format;
  desc.width = width;
  desc.height = height;
  desc.render_target = !depth;
  desc.depth_stencil = depth;
  // §16.123: every image made here is EDRAM-derived (targets, resolves, their copies). §16.124: except the output-sized
  // ones (1280 wide: the front buffer the UI is drawn into and its resolves) - the game composites its 1200x704 scene
  // into them with its own final pass, which then upscales the scaled scene while the UI stays at full resolution.
  desc.scaled = width < 1280;
  image.gpu = backend_->CreateImage(desc);
  image.desc = desc;
  if (!image.gpu) {
    REXGPU_ERROR("[sdnx_native] could not create a {}x{} image (format {})", width, height, int(format));
    return false;
  }
  image.format = format;
  image.width = width;
  image.height = height;
  image.initialized = false;
  if (trace_image) {
    REXGPU_INFO("[sdnx_trace]   image ready");
  }
  return true;
}

void NativeCommandProcessor::DestroyImage(Image& image) {
  if (image.gpu) backend_->DestroyImage(image.gpu);
  image = Image();
}

void NativeCommandProcessor::InitializeLayout(Image& image) {
  if (image.initialized) return;
  if (!image.gpu) return;
  backend_->InitializeImage(image.gpu, image.desc, true);
  transfer_pending_ = true;  // see BeginPass: the next rendering pass needs a full barrier
  FullBarrier();
  image.initialized = true;
}

void NativeCommandProcessor::RetireImage(Image& image) {
  if (image.gpu) {
    // Into the slot of the frame being recorded: open it first, otherwise the image would land in the slot
    // EnsureFrame empties right away while the previous frame may still be using it.
    EnsureFrame();
    RetireViews(image.gpu);
    retired_images_[uint32_t(frame_ % kFramesInFlight)].push_back(image);
  }
  image = Image();
}

void NativeCommandProcessor::FullBarrier(int line, const char* file) {
  ++stats_.full_barriers;
  static const bool prof = std::getenv("SDNX_PROF") != nullptr;
  if (prof) {
    const char* base = std::strrchr(file, '/');
    const char* base2 = std::strrchr(file, '\\');
    base = base2 > base ? base2 : base;
    ++barrier_sites_[std::string(base ? base + 1 : file) + ":" + std::to_string(line)];
  }
  backend_->FullBarrier();
  // Everything recorded before is ordered with everything after: no pending transfer, no attachment hazard left.
  transfer_pending_ = false;
  attachments_written_.clear();
}

// Between rendering passes with nothing but draws since the last barrier: only the attachments of the earlier passes
// have to be written before this pass reads or writes them (depth test, blending, load). Sampled textures come from
// transfers (resolves, uploads), which set transfer_pending_ and get the full barrier.
void NativeCommandProcessor::AttachmentBarrier() {
  ++stats_.attachment_barriers;
  backend_->AttachmentBarrier();
  attachments_written_.clear();
}

// The barrier a new rendering pass needs: full after a transfer, attachment-only when it uses an attachment an
// earlier pass wrote since the last barrier, none otherwise (338 full barriers per frame before, 2026-11-13).
// SDNX_FULL_BARRIERS=1 restores a full barrier before every pass (A/B).
void NativeCommandProcessor::BeginPassBarrier(GpuImage* const* images, uint32_t count) {
  static const bool always_full = std::getenv("SDNX_FULL_BARRIERS") != nullptr;
  bool overlap = false;
  for (uint32_t i = 0; i < count && !overlap; ++i) {
    if (!images[i]) continue;
    for (const GpuImage* written : attachments_written_) {
      if (written == images[i]) {
        overlap = true;
        break;
      }
    }
  }
  if (always_full || transfer_pending_) {
    FullBarrier();
  } else if (overlap) {
    AttachmentBarrier();
  } else {
    ++stats_.pass_barriers_skipped;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (images[i]) attachments_written_.push_back(images[i]);
  }
}

NativeCommandProcessor::Image* NativeCommandProcessor::GetRenderTarget(uint32_t base_tiles, uint32_t pitch_pixels,
                                                                       uint32_t format, xenos::MsaaSamples msaa,
                                                                       bool is_depth, uint32_t min_height) {
  // Images are in EDRAM samples: 2X MSAA doubles the height, 4X both dimensions. D3D clears at half the
  // resolution with 4X MSAA (pitch 640 for a 1280 surface) - the same EDRAM tiles, so the same image; keyed by
  // the pixel pitch and MSAA, those clears went to an image of their own and the UI stencil was never cleared.
  uint32_t sx = uint32_t(msaa >= xenos::MsaaSamples::k4X), sy = uint32_t(msaa >= xenos::MsaaSamples::k2X);
  pitch_pixels = std::max(pitch_pixels << sx, uint32_t(8));
  // The cache and allocation must use the same sample-space height. A full-height
  // scissor with MSAA can request 4096 rows even when the existing 2048-row image fits.
  constexpr uint32_t kMaxHeightSamples = 2048;
  const uint32_t requested_height = min_height;
  min_height = std::max(8u, std::min(min_height, kMaxHeightSamples >> sy) << sy);
  uint64_t key = RenderTargetKey(base_tiles, pitch_pixels, format, xenos::MsaaSamples::k1X, is_depth);
  Image& image = render_targets_[key];
  if (image.gpu && image.height >= min_height) {
    return &image;
  }
  const uint32_t height = (min_height + 15) & ~15u;
  static uint32_t rt_count = 0;
  if (rt_count < 200) {
    REXGPU_INFO("[sdnx_rt] #{} base={:#x} pitch={} format={} depth={} msaa={} requested_rows={} "
                "sample_rows={} -> {}",
                rt_count++, base_tiles, pitch_pixels, format, is_depth, uint32_t(msaa), requested_height,
                image.height, height);
  }
  if (image.gpu) {
    // Grow: the old contents are dropped (rare; the next stage keeps them). The image may still be used by
    // commands recorded in this frame, so it is destroyed later.
    RetireImage(image);
  }
  // Creating the image records an initialization barrier, and barriers must not be recorded inside a
  // render pass; also, after a submit cb_ is null until the next frame is opened, so make sure a command
  // buffer is recording first (recording into the null handle crashed the NVIDIA driver).
  EnsureFrame();
  EndRendering();
  VkFormat vk_format =
      is_depth ? VK_FORMAT_D32_SFLOAT_S8_UINT : ColorTargetFormat(xenos::ColorRenderTargetFormat(format));
  static const bool trace_rt = std::getenv("SDNX_TRACE_DRAW") != nullptr;
  if (trace_rt) {
    REXGPU_INFO("[sdnx_trace] render target base={} pitch={} guest_format={} depth={} height={} -> {}", base_tiles,
                pitch_pixels, format, is_depth, height, int(vk_format));
  }
  if (!CreateImage(image, vk_format, pitch_pixels, height, is_depth)) {
    render_targets_.erase(key);
    return nullptr;
  }
  InitializeLayout(image);
  return &image;
}

NativeCommandProcessor::Image* NativeCommandProcessor::GetResolved(uint32_t address, VkFormat format,
                                                                   uint32_t width, uint32_t height) {
  width = std::max(width, uint32_t(8));
  height = std::max(height, uint32_t(8));
  Image& image = resolved_[address];
  if (image.gpu && image.format == format && image.width >= width && image.height >= height) {
    return &image;
  }
  if (image.gpu) RetireImage(image);
  ++resolved_layout_epoch_;  // §16.168
  EnsureFrame();
  EndRendering();
  if (!CreateImage(image, format, width, height, false)) {
    resolved_.erase(address);
    return nullptr;
  }
  InitializeLayout(image);
  return &image;
}

bool NativeCommandProcessor::SetupContext() {
  static_assert(kFramesInFlight == GpuBackend::kFramesInFlight);
#if REX_PLATFORM_SWITCH
  // deko3d is opt-in until its presenter is in (docs/deko3d-backend.md); until then the console runs the Vulkan
  // (NVK) path. SDNX_DEKO3D=1 or the cvar turns it on.
  static const bool use_deko3d = [] {
    const char* env = std::getenv("SDNX_DEKO3D");
    return (env && *env && *env != '0') || REXCVAR_GET(sdnx_deko3d_backend);
  }();
  backend_ = use_deko3d ? CreateDeko3dBackend() : CreateVulkanBackend(graphics_system_);
  REXGPU_INFO("[sdnx_native] backend: {} (SDNX_DEKO3D={})", use_deko3d ? "deko3d" : "Vulkan",
              std::getenv("SDNX_DEKO3D") ? std::getenv("SDNX_DEKO3D") : "-");
#else
  backend_ = CreateVulkanBackend(graphics_system_);
#endif
  if (!backend_->Initialize()) {
    REXGPU_ERROR("[sdnx_native] could not start the {} backend", backend_->name());
    return false;
  }
  if (!InitializeDraws()) {
    REXGPU_ERROR("[sdnx_native] could not create the draw resources");
    return false;
  }
  stats_start_ = std::chrono::steady_clock::now();
  REXGPU_INFO("[sdnx_native] context ready ({} frames in flight, EDRAM-lite, resolves, output)", kFramesInFlight);
  return true;
}

void NativeCommandProcessor::ShutdownContext() {
  backend_->WaitIdle();
  ShutdownQueries();
  ShutdownGeometry();
  ShutdownDraws();
  for (auto& slot_images : retired_images_) {
    for (Image& retired : slot_images) DestroyImage(retired);
    slot_images.clear();
  }
  for (auto& it : render_targets_) DestroyImage(it.second);
  render_targets_.clear();
  DestroyImage(depth_resolve_temp_);
  for (auto& it : resolved_stencil_) DestroyImage(it.second);
  resolved_stencil_.clear();
  for (auto& it : resolved_) DestroyImage(it.second);
  resolved_.clear();
  for (LumaSlot& slot : luma_slots_) {
    backend_->DestroyBuffer(slot.buffer);
    slot = LumaSlot();
  }
  backend_->DestroyBuffer(dump_buffer_);
  dump_buffer_ = nullptr;
  dump_mapping_ = nullptr;
  backend_->Shutdown();
  backend_.reset();
  shaders_.clear();
}

void NativeCommandProcessor::EnsureFrame() {
  if (frame_open_) return;
  uint32_t slot = uint32_t(frame_ % kFramesInFlight);
  backend_->BeginFrame(slot);  // waits for the slot's fence
  LumaCheck(slot);  // the slot's previous frame has completed: its presented image is in the readback buffer
  transfer_pending_ = true;  // first pass of a command buffer: full barrier (earlier submissions)
  upload_[slot].offset = 0;
  frame_open_ = true;
  ResetQueries(slot);  // the slot's previous frame has completed: its occlusion counts are readable
  ReleaseRetiredGeometry(slot);
  callback_frame_.store(uint32_t(frame_) + 1, std::memory_order_relaxed);
  ReleaseRetiredViews(slot);
  for (Image& retired : retired_images_[slot]) DestroyImage(retired);
  retired_images_[slot].clear();
  EvictTextures();  // after the release above: what it retires is destroyed when this slot comes round again
  if (!dummies_initialized_) {
    // The placeholder textures: GENERAL layout (as their descriptors say) and white. They were left in
    // UNDEFINED, so the shaders sampled undefined data (zero on NVIDIA): textured UI came out black.
    dummies_initialized_ = true;
    SamplerDesc sampler;
    sampler.mag_linear = sampler.min_linear = true;
    backend_->WriteSamplerDescriptor(0, sampler);
    Image* dummies[] = {&dummy_2d_, &dummy_3d_, &dummy_cube_};
    for (uint32_t i = 0; i < 3; ++i) {
      Image* dummy = dummies[i];
      backend_->WriteImageDescriptor(dummy->desc.type, 0, dummy_views_[i]);
      backend_->InitializeImage(dummy->gpu, dummy->desc, false);
      ImageRegion all;
      all.layer_count = dummy == &dummy_cube_ ? 6 : 1;
      const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
      backend_->ClearColor(dummy->gpu, all, white);
      transfer_pending_ = true;  // see BeginPass: the next rendering pass needs a full barrier
      dummy->initialized = true;
    }
    FullBarrier();
  }
}

bool NativeCommandProcessor::SubmitFrame(const Image* front, uint32_t width, uint32_t height) {
  if (!frame_open_) return true;
  EndRendering();
  const uint32_t slot = uint32_t(frame_ % kFramesInFlight);
  CopyQueryResults(slot);
  bool result = true;
  bool presented = false;
  if (front && front->gpu) {
    FullBarrier();  // everything drawn before the copy of the front image
    presented = backend_->Present(slot, front->gpu, front->width, front->height, width, height,
                                  front->format == VK_FORMAT_R8G8B8A8_UNORM);
  }
  if (presented) {
    static bool first = true;
    if (first) {
      first = false;
      REXGPU_INFO("[sdnx_native] output: frames are presented through the {} backend", backend_->name());
    }
  } else {
    if (front && front->gpu) {
      static uint32_t not_presented = 0;
      if (not_presented++ < 4) REXGPU_WARN("[sdnx_native] output: the presenter took no frame (swap {})", swap_count_);
    }
    result = backend_->SubmitFrame(slot);
  }
  frame_open_ = false;
  ++frame_;
  return result;
}

Shader* NativeCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                           const uint32_t* host_address, uint32_t dword_count) {
  ++stats_.shader_loads;
  // The game loads the microcode before nearly every draw (IM_LOAD, ~2000 times a frame on the console), and every
  // load hashed the whole microcode. A load from guest memory (not one inline in the ring) at an address whose pages
  // have not been written since the last load there returns the same shader without hashing: the pages are watched
  // like a texture's (armed, version taken, then hashed). Pages written again soon after every arm are left alone.
  struct ShaderLoad {
    Shader* shader = nullptr;
    uint64_t version = 0, armed_frame = 0;
    uint32_t page_first = 0, page_count = 0, rearms = 0;
    bool unwatched = false, armed = false;
  };
  static FlatMap64<ShaderLoad> loads;
  static const bool no_cache = std::getenv("SDNX_NO_SHADERCACHE") != nullptr;  // A/B measurements
  ShaderLoad* load = nullptr;
  if (!no_cache && dword_count && memory_->TranslatePhysical<const uint32_t*>(guest_address) == host_address) {
    uint64_t lkey = uint64_t(guest_address) | uint64_t(dword_count) << 32 | uint64_t(shader_type) << 60;
    lkey = (lkey ^ (lkey >> 29)) * 0xBF58476D1CE4E5B9ull;  // spread: the table indexes by the low bits
    load = loads.try_emplace(lkey ^ (lkey >> 32)).first;
    if (load->armed && !load->unwatched && PagesVersion(load->page_first, load->page_count) == load->version) {
      ++stats_.shader_load_hits;
      return load->shader;
    }
    // Pages written again since the watch was armed hold rewritten microcode (a ring, or shaders of other sizes at
    // the same addresses every frame): those pages are not watched again, for any load, as watching them only adds
    // write faults.
    static FlatMap64<uint8_t> rewritten_pages;
    auto page_key = [](uint32_t page) { return (uint64_t(page) + 1) * 0x9E3779B97F4A7C15ull; };
    if (!load->unwatched && load->armed) {
      for (uint32_t p = 0; p < load->page_count; ++p) rewritten_pages.try_emplace(page_key(load->page_first + p));
      load->unwatched = true;
    }
    if (!load->unwatched && load->shader) {
      const uint32_t first = (guest_address & 0x1FFFFFFFu) >> 12;
      const uint32_t last = ((guest_address & 0x1FFFFFFFu) + dword_count * 4 - 1) >> 12;
      for (uint32_t p = first; p <= last && !load->unwatched; ++p) {
        if (rewritten_pages.find(page_key(p))) load->unwatched = true;
      }
    }
    // Watched from the second load at this address on: microcode placed at a new address every frame (a ring)
    // would only cost a watch and a write fault per load.
    if (!load->unwatched && load->shader) {
      load->armed = true;
      const uint32_t bytes = dword_count * sizeof(uint32_t);
      load->page_first = (guest_address & 0x1FFFFFFFu) >> 12;
      load->page_count = (((guest_address & 0x1FFFFFFFu) + bytes - 1) >> 12) - load->page_first + 1;
      memory_->EnablePhysicalMemoryAccessCallbacks(guest_address, bytes, true, false);
      load->version = PagesVersion(load->page_first, load->page_count);
      load->armed_frame = frame_;
    }
  }
  uint64_t hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  Shader* result;
  auto it = shaders_.find(hash);
  if (it != shaders_.end()) {
    result = it->second.get();
  } else {
    ++stats_.shader_new;
    auto shader = std::make_unique<Shader>(shader_type, hash, host_address, dword_count);
    result = shader.get();
    shaders_.emplace(hash, std::move(shader));
  }
  if (load) load->shader = result;
  return result;
}

namespace {
uint64_t SdnxNowNs() { return SdnxTicksToNs(SdnxTicks()); }  // §16.141
}  // namespace

void NativeCommandProcessor::PrepareForWait() { cpu_wait_start_ns_ = SdnxNowNs(); }

void NativeCommandProcessor::ReturnFromWait() {
  if (cpu_wait_start_ns_) cpu_idle_ns_ += SdnxNowNs() - cpu_wait_start_ns_;
  cpu_wait_start_ns_ = 0;
}

bool NativeCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info, bool major_mode_explicit) {
  (void)prim_type;
  (void)index_buffer_info;
  (void)major_mode_explicit;
  const uint64_t start = SdnxNowNs();
  // A resolve arrives as a draw in the EDRAM copy mode (as in the Vulkan backend).
  if (register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) {
    const bool ok = IssueCopy();
    cpu_copy_ns_ += SdnxNowNs() - start;
    return ok;
  }
  ++stats_.draws;
  ++cpu_window_draws_;
  stats_.indices += index_count;
  const bool ok = DrawNative(prim_type, index_count, index_buffer_info);
  cpu_draw_ns_ += SdnxNowNs() - start;
  return ok;
}

// §16.172: see TileMerge in the header.
NativeCommandProcessor::TileRole NativeCommandProcessor::TileMergeClassify(const RegisterFile& regs) {
  const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  const auto window = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
  const int32_t wo_x = window.window_x_offset, wo_y = window.window_y_offset;
  if (surface.msaa_samples != xenos::MsaaSamples::k1X || wo_x != 0) {
    tile_merge_.phase = 0;
    return TileRole::kNone;
  }
  const uint32_t pitch = surface.surface_pitch;
  const uint32_t depth_base = regs.Get<reg::RB_DEPTH_INFO>().depth_base;
  const uint32_t color_base0 = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]).color_base;
  TileMerge& t = tile_merge_;
  const bool same = t.known && pitch == t.pitch && depth_base == t.depth_base && color_base0 == t.color_base0;
  if (wo_y < 0) {
    draw_util::Scissor sc;
    draw_util::GetScissor(regs, sc);
    if (sc.offset[1] != 0 || !sc.extent[1]) {
      t.phase = 0;
      return TileRole::kNone;
    }
    if (!same || t.tile_h != sc.extent[1]) {
      t = TileMerge();
      t.known = true;
      t.pitch = pitch;
      t.depth_base = depth_base;
      t.color_base0 = color_base0;
      t.tile_h = sc.extent[1];
      const uint32_t mask = regs[XE_GPU_REG_RB_COLOR_MASK];
      for (uint32_t i = 0; i < 4; ++i) {
        if (!((mask >> (4 * i)) & 0xF)) continue;
        t.color_bases[t.color_base_count++] =
            regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]).color_base;
      }
      REXGPU_INFO("[sdnx_tiles] tiled pass: color base {} depth base {} pitch {}, tile {} rows, window y {}",
                  color_base0, depth_base, pitch, t.tile_h, wo_y);
    }
    t.full_h = std::max(t.full_h, uint32_t(-wo_y) + t.tile_h);
    t.seen_frame = frame_;
    if (t.expanded_frame == frame_) {
      t.phase = 2;
      t.tile_offset = uint32_t(-wo_y);
      t.skipped_frame = frame_;
      return TileRole::kSkip;
    }
    t.phase = 0;
    return TileRole::kNone;
  }
  if (wo_y == 0 && same && t.seen_frame != UINT64_MAX && t.seen_frame != frame_ && frame_ - t.seen_frame <= 2 &&
      t.skipped_frame != frame_) {
    t.phase = 1;
    t.expanded_frame = frame_;
    return TileRole::kFirst;
  }
  t.phase = 0;
  return TileRole::kNone;
}

// §16.174: inside the first tile of a merged pass every draw is drawn (that tile now covers the whole frame; a draw the
// game predicated to the lower tile only would otherwise be lost with the skipped lower tile).
bool NativeCommandProcessor::ForcePredicatedDraw() {
  static const bool tile_merge = REXCVAR_GET(sdnx_tile_merge);
  const TileMerge& t = tile_merge_;
  if (!tile_merge || !t.known || t.seen_frame == UINT64_MAX || t.seen_frame == frame_ || frame_ - t.seen_frame > 2 ||
      t.skipped_frame == frame_) {
    return false;
  }
  const RegisterFile& regs = *register_file_;
  const auto window = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
  if (window.window_x_offset != 0 || window.window_y_offset != 0) return false;
  const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  return surface.msaa_samples == xenos::MsaaSamples::k1X && surface.surface_pitch == t.pitch &&
         regs.Get<reg::RB_DEPTH_INFO>().depth_base == t.depth_base &&
         regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]).color_base == t.color_base0;
}

bool NativeCommandProcessor::TileMergeTarget(bool depth, uint32_t base, uint32_t pitch) const {
  const TileMerge& t = tile_merge_;
  if (!t.known || pitch != t.pitch) return false;
  if (depth) return base == t.depth_base;
  for (uint32_t i = 0; i < t.color_base_count; ++i) {
    if (t.color_bases[i] == base) return true;
  }
  return false;
}

bool NativeCommandProcessor::IssueCopy() {
  ++stats_.copies;
  ++resolve_epoch_;
  static const bool skip_resolves = std::getenv("SDNX_NO_RESOLVE") != nullptr;
  if (skip_resolves) {
    return true;
  }
  draw_util::ResolveInfo info;
  if (!draw_util::GetResolveInfo(*register_file_, *memory_, 1, 1, false, false, info)) {
    return true;
  }
  if (!info.coordinate_info.width_div_8 || !info.height_div_8) {
    return true;
  }
  EnsureFrame();
  EndRendering();
  const RegisterFile& regs = *register_file_;
  auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  uint32_t pitch = surface.surface_pitch;
  xenos::MsaaSamples msaa = surface.msaa_samples;
  bool copy_depth = info.IsCopyingDepth();

  // The rectangle in surface pixels, from the packed EDRAM base offset (see draw_util::GetResolveInfo).
  const draw_util::ResolveEdramInfo& edram = copy_depth ? info.depth_edram_info : info.color_edram_info;
  uint32_t original_base = copy_depth ? info.depth_original_base : info.color_original_base;
  uint32_t sx = uint32_t(msaa >= xenos::MsaaSamples::k4X), sy = uint32_t(msaa >= xenos::MsaaSamples::k2X);
  uint32_t pitch_tiles = std::max(xenos::GetSurfacePitchTiles(pitch, msaa, false), 1u);
  uint32_t offset_tiles = (edram.base_tiles - original_base) >> (copy_depth ? 0 : edram.format_is_64bpp);
  uint32_t x0 = ((offset_tiles % pitch_tiles) * xenos::kEdramTileWidthSamples +
                 (info.coordinate_info.edram_offset_x_div_8 << (sx + 3))) >> sx;
  uint32_t y0 = ((offset_tiles / pitch_tiles) * xenos::kEdramTileHeightSamples +
                 (info.coordinate_info.edram_offset_y_div_8 << (sy + 3))) >> sy;
  uint32_t width = info.coordinate_info.width_div_8 * 8, height = info.height_div_8 * 8;
  // The EDRAM-to-pixel math can produce a rectangle far outside the surface (a 1280x4096 target for a 720p frame
  // grew the render target and the front-buffer blit every frame, and the GPU faulted after ~35 of them).
  {
    static uint32_t res_count = 0;
    if (res_count < 20 || (x0 + width > 2048 || y0 + height > 2048)) {
      REXGPU_INFO("[sdnx_res] #{} base={:#x} x={} y={} w={} h={} pitch={} msaa={} depth={}", res_count,
                  original_base, x0, y0, width, height, pitch, uint32_t(msaa), copy_depth);
    }
    ++res_count;
  }
  // §16.172: a resolve of a merged tiled pass. After the skipped later tile: its rows of the whole image. After the
  // first tile (drawn whole): no clears yet - they would wipe the rows the later tiles' resolves still read.
  bool tile_skip_clears = false;
  {
    static const bool tile_merge = REXCVAR_GET(sdnx_tile_merge);
    if (tile_merge && surface.msaa_samples == xenos::MsaaSamples::k1X &&
        TileMergeTarget(copy_depth, original_base, pitch)) {
      if (tile_merge_.phase == 2) {
        y0 += tile_merge_.tile_offset;
        ++stats_.tile_resolves_moved;
      } else if (tile_merge_.expanded_frame == frame_ && tile_merge_.skipped_frame != frame_) {
        tile_skip_clears = true;
      }
    }
  }
  if (x0 + width > 4096 || y0 + height > 2048) {
    // Do not make an oversized target or blit from it: the guest resolves what fits its surface.
    ++stats_.copies_incompatible;
    return true;
  }

  uint32_t format =
      copy_depth ? uint32_t(regs.Get<reg::RB_DEPTH_INFO>().depth_format) : uint32_t(edram.format);
  Image* source = GetRenderTarget(original_base, pitch, format, msaa, copy_depth, y0 + height);

  // Copy.
  if (source && info.copy_dest_extent_length) {
    if (copy_depth) {
      ++stats_.copies_depth;
      ResolveDepth(*source, info, x0, y0, width, height, sx, sy);
    } else {
      uint32_t dest_base = regs[XE_GPU_REG_RB_COPY_DEST_BASE] & 0x1FFFFFFF;
      uint32_t dest_w = info.copy_dest_coordinate_info.pitch_aligned_div_32 * 32;
      uint32_t dest_h = info.copy_dest_coordinate_info.height_aligned_div_32 * 32;
      VkFormat dest_format =
          ResolveDestFormat(xenos::TextureFormat(info.copy_dest_info.copy_dest_format), source->format);
      // The guest asks for a different format here (HDR 7e3 -> 16_16_16_16), but vkCmdBlitImage only
      // accepts blit-compatible formats. Allocate the resolved texture in the source format so the copy is
      // legal; the value encoding of the guest format is a separate conversion step (compute shader).
      // The rectangle lands at the destination offset (not at its render target position).
      uint32_t dx = info.copy_dest_coordinate_info.offset_x_div_8 * 8;
      uint32_t dy = info.copy_dest_coordinate_info.offset_y_div_8 * 8;
      // The image is as large as what is copied into it, not the pitch-aligned destination (608 for a 600 wide
      // resolve): a texture of the resolved size then samples it directly; with the padded size every such texture
      // was assembled into a copy every frame (~41 copies, each a rendering pass break and two full barriers).
      static const bool padded = std::getenv("SDNX_PADDED_RESOLVES") != nullptr;  // A/B
      Image* dest = padded ? GetResolved(dest_base, source->format, std::max(dest_w, dx + width),
                                         std::max(dest_h, dy + height))
                           : GetResolved(dest_base, source->format, dx + width, dy + height);
      resolved_extent_[dest_base] = info.copy_dest_extent_start + info.copy_dest_extent_length - dest_base;
      if (log_frame_) {
        REXGPU_INFO("[sdnx_frame] resolve rt base={} fmt={} {}x{} at {},{} -> {:#x} at {},{} fmt={} swap={} clear "
                    "color={} depth={}",
                    original_base, format, width, height, x0, y0, dest_base, dx, dy,
                    uint32_t(info.copy_dest_info.copy_dest_format), uint32_t(info.copy_dest_info.copy_dest_swap),
                    uint32_t(info.rb_copy_control.color_clear_enable), uint32_t(info.rb_copy_control.depth_clear_enable));
      }
      static const bool trace_resolve = std::getenv("SDNX_TRACE_RESOLVE") != nullptr;
      static std::unordered_map<uint64_t, int> traced_resolves;
      uint64_t trace_key = uint64_t(dest_base) ^ (uint64_t(format) << 32) ^ (uint64_t(width) << 40) ^
                           (uint64_t(uint32_t(info.copy_dest_info.value)) << 20);
      if (trace_resolve && traced_resolves[trace_key]++ == 0) {
        REXGPU_INFO("[sdnx_resolve] rt base={} fmt={} host={} {}x{} at {},{} -> dest {:#x} fmt={} number={} "
                    "exp_bias={} dest {}x{} img {}x{} dest offset {},{}",
                    original_base, format, int(source->format), width, height, x0, y0, dest_base,
                    uint32_t(info.copy_dest_info.copy_dest_format), uint32_t(info.copy_dest_info.copy_dest_number),
                    int32_t(info.copy_dest_info.copy_dest_exp_bias), dest_w, dest_h, dest ? dest->width : 0,
                    dest ? dest->height : 0, dx, dy);
      }
      (void)dest_format;
      ++resolved_generation_[dest_base];
      resolved_swap_[dest_base] = info.copy_dest_info.copy_dest_swap != 0;
      resolved_frame_[dest_base] = frame_;
      if (dest) {
        // vkCmdBlitImage needs blit-compatible formats; the HDR 7e3 -> 16F resolve is not, and an
        // incompatible blit is invalid usage (it took the NVIDIA driver down). Skip just the copy and count
        // it; the compute-shader conversion is the proper fix (next stage).
        static const bool trace_copy = std::getenv("SDNX_TRACE_DRAW") != nullptr;
        if (source->format != dest->format) {
          ++stats_.copies_incompatible;
          if (trace_copy) {
            REXGPU_INFO("[sdnx_trace] resolve skipped: {} -> {} ({}x{} at {},{}), dest base {:#x}",
                        int(source->format), int(dest->format), width, height, x0, y0, dest_base);
          }
        } else {
          FullBarrier();
          if (trace_copy) {
            REXGPU_INFO("[sdnx_trace] resolve src img={} base={:#x} fmt={} pitch={} msaa={} drawn={} -> dest={:#x} "
                        "{}x{}",
                        reinterpret_cast<uintptr_t>(source), original_base, int(source->format), pitch,
                        uint32_t(msaa), uint32_t(source->drawn), dest_base, width, height);
          }
          // The source is in samples (MSAA surfaces are larger): the blit downsamples.
          uint32_t src_x = x0 << sx, src_y = y0 << sy;
          uint32_t copy_w = (std::min((x0 + width) << sx, source->width) - std::min(src_x, source->width)) >> sx;
          uint32_t copy_h = (std::min((y0 + height) << sy, source->height) - std::min(src_y, source->height)) >> sy;
          copy_w = std::min(copy_w, dest->width > dx ? dest->width - dx : 0u);
          copy_h = std::min(copy_h, dest->height > dy ? dest->height - dy : 0u);
          if (copy_w && copy_h) {
            ImageRegion from, to;
            from.x = int32_t(src_x);
            from.y = int32_t(src_y);
            from.width = copy_w << sx;
            from.height = copy_h << sy;
            to.x = int32_t(dx);
            to.y = int32_t(dy);
            to.width = copy_w;
            to.height = copy_h;
            backend_->BlitImage(source->gpu, from, dest->gpu, to, Aspect::kColor, sx || sy);
            transfer_pending_ = true;  // see BeginPass: the next rendering pass needs a full barrier
            if (log_frame_) backend_->DebugProbe(dest->gpu, to, dest_base, "resolve");  // §16.108
          }
        }
        DumpResolveIfRequested(dest_base, *dest);
      }
    }
  }

  // Clears requested by the resolve.
  if (tile_skip_clears) {  // §16.172
    if (info.rb_copy_control.color_clear_enable || info.rb_copy_control.depth_clear_enable) {
      ++stats_.tile_clears_skipped;
    }
    return true;
  }
  auto copy_control = info.rb_copy_control;
  if (copy_control.color_clear_enable && !copy_depth) {
    Image* target = GetRenderTarget(info.color_original_base, pitch, uint32_t(info.color_edram_info.format), msaa,
                                    false, y0 + height);
    if (target) {
      FullBarrier();
      VkClearColorValue color = DecodeColorClear(xenos::ColorRenderTargetFormat(info.color_edram_info.format),
                                                 info.rb_color_clear, info.rb_color_clear_lo);
      // vkCmdClearColorImage clears the whole image; the resolve rectangle is the whole target in practice.
      // EDRAM is one memory: a clear through one format also clears the surfaces of other formats at the same
      // base and pitch. Images are per format here, so clear all of them - otherwise what was drawn through
      // another format stays (the menu selection highlight accumulated on every item it passed over).
      uint64_t base_pitch = uint64_t(info.color_original_base & 0xFFF) | (uint64_t((pitch << sx) & 0xFFFF) << 12);
      for (auto& it : render_targets_) {
        bool is_depth = (it.first >> 38) & 1;
        if (is_depth || (it.first & 0xFFFFFFF) != base_pitch || !it.second.gpu) continue;
        backend_->ClearColor(it.second.gpu, ImageRegion(), color.float32);
        transfer_pending_ = true;  // see BeginPass: the next rendering pass needs a full barrier
        ++stats_.clears;
      }
    }
  }
  if (copy_control.depth_clear_enable) {
    Image* target = GetRenderTarget(info.depth_original_base, pitch,
                                    uint32_t(regs.Get<reg::RB_DEPTH_INFO>().depth_format), msaa, true, y0 + height);
    if (target) {
      FullBarrier();
      backend_->ClearDepthStencil(target->gpu, ImageRegion(), Aspect::kDepthStencil,
                                  float(info.rb_depth_clear >> 8) / float(0xFFFFFF), info.rb_depth_clear & 0xFF);
      transfer_pending_ = true;  // see BeginPass: the next rendering pass needs a full barrier
      ++stats_.clears;
    }
  }
  return true;
}

void NativeCommandProcessor::ResolveDepth(const Image& source, const draw_util::ResolveInfo& info, uint32_t x0,
                                          uint32_t y0, uint32_t width, uint32_t height, uint32_t sx,
                                          uint32_t sy) {
  uint32_t dest_base = (*register_file_)[XE_GPU_REG_RB_COPY_DEST_BASE] & 0x1FFFFFFF;
  uint32_t dest_w = info.copy_dest_coordinate_info.pitch_aligned_div_32 * 32;
  uint32_t dest_h = info.copy_dest_coordinate_info.height_aligned_div_32 * 32;
  uint32_t dx = info.copy_dest_coordinate_info.offset_x_div_8 * 8;
  uint32_t dy = info.copy_dest_coordinate_info.offset_y_div_8 * 8;
  Image* dest = GetResolved(dest_base, VK_FORMAT_R32_SFLOAT, std::max(dest_w, dx + width),
                            std::max(dest_h, dy + height));
  ++resolved_generation_[dest_base];
  resolved_frame_[dest_base] = frame_;
  resolved_extent_[dest_base] = info.copy_dest_extent_start + info.copy_dest_extent_length - dest_base;
  if (log_frame_) {
    REXGPU_INFO("[sdnx_frame] depth resolve (depth format {}) {}x{} at {},{} (samples x{} y{}) -> {:#x} at {},{}",
                uint32_t(register_file_->Get<reg::RB_DEPTH_INFO>().depth_format), width, height, x0,
                y0, 1u << sx, 1u << sy, dest_base, dx, dy);
  }
  if (!dest) return;
  // Source rectangle in samples, clamped to the image.
  uint32_t src_x = x0 << sx, src_y = y0 << sy;
  if (src_x >= source.width || src_y >= source.height) return;
  uint32_t src_w = std::min(width << sx, source.width - src_x), src_h = std::min(height << sy, source.height - src_y);
  uint32_t out_w = std::min(src_w >> sx, dest->width > dx ? dest->width - dx : 0u);
  uint32_t out_h = std::min(src_h >> sy, dest->height > dy ? dest->height - dy : 0u);
  if (!out_w || !out_h) return;
  src_w = out_w << sx;
  src_h = out_h << sy;
  // §16.165: without MSAA the backend may copy the extracted depth and stencil straight into the images.
  if (!sx && !sy) {
    Image& stencil = resolved_stencil_[dest_base];
    if (stencil.gpu && (stencil.width < dest->width || stencil.height < dest->height)) RetireImage(stencil);
    if (!stencil.gpu && CreateImage(stencil, VK_FORMAT_R8_UNORM, dest->width, dest->height, false)) {
      InitializeLayout(stencil);
    }
    ImageRegion from;
    from.x = int32_t(src_x);
    from.y = int32_t(src_y);
    from.width = src_w;
    from.height = src_h;
    ImageRegion to = from;
    to.x = int32_t(dx);
    to.y = int32_t(dy);
    EndRendering();
    FullBarrier();
    if (backend_->ExtractDepthStencilToImages(source.gpu, from, dest->gpu, to, stencil.gpu, to)) {
      transfer_pending_ = true;  // see BeginPass: the next rendering pass needs a full barrier
      FullBarrier();
      if (log_frame_) backend_->DebugProbe(dest->gpu, to, dest_base, "depth");
      return;
    }
  }
  VkDeviceSize offset = Upload(nullptr, VkDeviceSize(src_w) * src_h * 4, 16);
  if (offset == UINT64_MAX) return;
  GpuBuffer* buffer = upload_[uint32_t(frame_ % kFramesInFlight)].gpu;
  EndRendering();
  FullBarrier();
  ImageRegion region;
  region.x = int32_t(src_x);
  region.y = int32_t(src_y);
  region.width = src_w;
  region.height = src_h;
  backend_->CopyImageToBuffer(source.gpu, region, Aspect::kDepth, {buffer, offset}, 0);
  transfer_pending_ = true;  // see BeginPass: the next rendering pass needs a full barrier
  FullBarrier();
  if (!sx && !sy) {
    region.x = int32_t(dx);
    region.y = int32_t(dy);
    backend_->CopyBufferToImage({buffer, offset}, 0, 0, dest->gpu, region, Aspect::kColor);
    transfer_pending_ = true;  // see BeginPass: the next rendering pass needs a full barrier
    // Stencil, for TryRestoreDepth.
    Image& stencil = resolved_stencil_[dest_base];
    if (stencil.gpu && (stencil.width < dest->width || stencil.height < dest->height)) RetireImage(stencil);
    if (!stencil.gpu && CreateImage(stencil, VK_FORMAT_R8_UNORM, dest->width, dest->height, false)) {
      InitializeLayout(stencil);
    }
    VkDeviceSize stencil_offset = stencil.gpu ? Upload(nullptr, VkDeviceSize(src_w) * src_h, 16) : UINT64_MAX;
    if (stencil_offset != UINT64_MAX) {
      FullBarrier();
      ImageRegion s_region;
      s_region.x = int32_t(src_x);
      s_region.y = int32_t(src_y);
      s_region.width = src_w;
      s_region.height = src_h;
      backend_->CopyImageToBuffer(source.gpu, s_region, Aspect::kStencil, {buffer, stencil_offset}, 0);
      transfer_pending_ = true;  // see BeginPass: the next rendering pass needs a full barrier
      FullBarrier();
      s_region.x = int32_t(dx);
      s_region.y = int32_t(dy);
      backend_->CopyBufferToImage({buffer, stencil_offset}, 0, 0, stencil.gpu, s_region, Aspect::kColor);
      transfer_pending_ = true;  // see BeginPass: the next rendering pass needs a full barrier
    }
  } else {
    if (!depth_resolve_temp_.gpu || depth_resolve_temp_.width < src_w || depth_resolve_temp_.height < src_h) {
      uint32_t tw = std::max(src_w, depth_resolve_temp_.width), th = std::max(src_h, depth_resolve_temp_.height);
      if (depth_resolve_temp_.gpu) RetireImage(depth_resolve_temp_);
      if (!CreateImage(depth_resolve_temp_, VK_FORMAT_R32_SFLOAT, tw, th, false)) return;
      InitializeLayout(depth_resolve_temp_);
    }
    region.x = 0;
    region.y = 0;
    backend_->CopyBufferToImage({buffer, offset}, 0, 0, depth_resolve_temp_.gpu, region, Aspect::kColor);
    transfer_pending_ = true;  // see BeginPass: the next rendering pass needs a full barrier
    FullBarrier();
    ImageRegion to;
    to.x = int32_t(dx);
    to.y = int32_t(dy);
    to.width = out_w;
    to.height = out_h;
    backend_->BlitImage(depth_resolve_temp_.gpu, region, dest->gpu, to, Aspect::kColor, false);
    transfer_pending_ = true;  // see BeginPass: the next rendering pass needs a full barrier
  }
  FullBarrier();
  if (log_frame_) {  // §16.108: what the depth resolve put into its R32F image (shadow maps: 0.79..1.0 on the stand)
    ImageRegion probed;
    probed.x = int32_t(dx);
    probed.y = int32_t(dy);
    probed.width = out_w;
    probed.height = out_h;
    backend_->DebugProbe(dest->gpu, probed, dest_base, "depth");
  }
}

bool NativeCommandProcessor::TryRestoreDepth(Shader* ps) {
  const RegisterFile& regs = *register_file_;
  if (!ps || regs.Get<reg::RB_MODECONTROL>().edram_mode != xenos::EdramMode::kColorDepth) return false;
  if ((regs[XE_GPU_REG_RB_COLOR_MASK] & 0xFFFF) != 0xF) return false;
  auto depth_control = regs.Get<reg::RB_DEPTHCONTROL>();
  if (depth_control.z_enable || depth_control.stencil_enable) return false;
  auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]);
  if (color_info.color_format != xenos::ColorRenderTargetFormat::k_8_8_8_8) return false;
  auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  if (surface.msaa_samples != xenos::MsaaSamples::k1X || !surface.surface_pitch) return false;
  static string::StringBuffer disassembly;
  if (!ps->is_ucode_analyzed()) ps->AnalyzeUcode(disassembly);
  if (ps->texture_bindings().size() != 1) return false;
  xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(ps->texture_bindings()[0].fetch_constant);
  if (fetch.type != xenos::FetchConstantType::kTexture || fetch.format != xenos::TextureFormat::k_8_8_8_8) return false;
  uint32_t base = (uint32_t(fetch.base_address) << 12) & 0x1FFFFFFF;
  auto first = resolved_.find(base);
  if (first == resolved_.end() || first->second.format != VK_FORMAT_R32_SFLOAT) return false;
  // The colour target must alias a depth buffer drawn to before (same EDRAM base and pitch).
  uint32_t pitch = surface.surface_pitch;
  if (render_targets_.find(RenderTargetKey(color_info.color_base, std::max(pitch, 8u), 0, xenos::MsaaSamples::k1X,
                                           true)) == render_targets_.end()) {
    return false;
  }
  uint32_t tw = fetch.size_2d.width + 1, th = fetch.size_2d.height + 1;
  Image* target = GetRenderTarget(color_info.color_base, pitch, 0, xenos::MsaaSamples::k1X, true, th);
  if (!target) return false;
  EnsureFrame();
  EndRendering();
  GpuBuffer* buffer = upload_[uint32_t(frame_ % kFramesInFlight)].gpu;
  uint32_t pitch_pixels = fetch.pitch ? uint32_t(fetch.pitch) << 5 : ((tw + 31) & ~31u);
  uint64_t row_bytes = uint64_t(pitch_pixels) * 4;
  // §16.114: the parts in address order, each cut at the row where the next part begins. With predicated tiling the
  // top tile's image (0x10233000) is as tall as the whole texture (the end-of-frame 1200x704 resolve goes there too),
  // so its lower rows held the previous frame's depth; restored after the bottom tile (0x103d5000, row 352) in
  // unordered_map order, they overwrote it - the lower half of the screen lost its depth and went black.
  std::vector<const std::pair<const uint32_t, Image>*> parts;
  for (const auto& part : resolved_) {
    if (!part.second.gpu || part.second.format != VK_FORMAT_R32_SFLOAT) continue;
    if (part.first < base || part.first >= base + row_bytes * th) continue;
    parts.push_back(&part);
  }
  std::sort(parts.begin(), parts.end(), [](auto* a, auto* b) { return a->first < b->first; });
  for (size_t index = 0; index < parts.size(); ++index) {
    const auto& part = *parts[index];
    uint64_t delta = part.first - base;
    uint32_t y = uint32_t(delta / row_bytes), x = uint32_t((delta % row_bytes) / 4);
    if (x >= tw || y >= th || x >= target->width || y >= target->height) continue;
    uint32_t w = std::min({tw - x, part.second.width, target->width - x});
    uint32_t h = std::min({th - y, part.second.height, target->height - y});
    if (index + 1 < parts.size()) {
      const uint32_t next_y = uint32_t((parts[index + 1]->first - base) / row_bytes);
      if (next_y > y) h = std::min(h, next_y - y);
    }
    VkDeviceSize offset = Upload(nullptr, VkDeviceSize(w) * h * 4, 16);
    if (offset == UINT64_MAX) continue;
    FullBarrier();
    ImageRegion region;
    region.width = w;
    region.height = h;
    backend_->CopyImageToBuffer(part.second.gpu, region, Aspect::kColor, {buffer, offset}, 0);
    transfer_pending_ = true;  // see BeginPass: the next rendering pass needs a full barrier
    FullBarrier();
    ImageRegion at = region;
    at.x = int32_t(x);
    at.y = int32_t(y);
    backend_->CopyBufferToImage({buffer, offset}, 0, 0, target->gpu, at, Aspect::kDepth);
    transfer_pending_ = true;  // see BeginPass: the next rendering pass needs a full barrier
    auto stencil = resolved_stencil_.find(part.first);
    if (stencil == resolved_stencil_.end() || !stencil->second.gpu) continue;
    w = std::min(w, stencil->second.width);
    h = std::min(h, stencil->second.height);
    offset = Upload(nullptr, VkDeviceSize(w) * h, 16);
    if (offset == UINT64_MAX) continue;
    region.width = w;
    region.height = h;
    backend_->CopyImageToBuffer(stencil->second.gpu, region, Aspect::kColor, {buffer, offset}, 0);
    transfer_pending_ = true;  // see BeginPass: the next rendering pass needs a full barrier
    FullBarrier();
    at = region;
    at.x = int32_t(x);
    at.y = int32_t(y);
    backend_->CopyBufferToImage({buffer, offset}, 0, 0, target->gpu, at, Aspect::kStencil);
    transfer_pending_ = true;  // see BeginPass: the next rendering pass needs a full barrier
  }
  FullBarrier();
  if (log_frame_) {
    REXGPU_INFO("[sdnx_frame] depth restore from {:#x} ({}x{}) to depth base {} pitch {} (image {:#x})", base, tw,
                th, uint32_t(color_info.color_base), pitch, uint64_t(reinterpret_cast<uintptr_t>(target->gpu)));
  }
  return true;
}

void NativeCommandProcessor::ReportStats() {
  auto now = std::chrono::steady_clock::now();
  double seconds = std::chrono::duration<double>(now - stats_start_).count();
  if (seconds < 5.0) return;
  double frames = std::max<double>(1.0, double(stats_.swaps));
  REXGPU_INFO(
      "[sdnx_native] {:.1f} s: {:.1f} swaps/s, {:.0f} draws/frame ({:.0f} indices/frame), {:.1f} resolves/frame "
      "({:.1f} depth), {:.1f} clears/frame, front buffer found {:.0f}%, shader loads {:.0f}/frame, new shaders {}, "
      "total shaders {}, render targets {}, resolved textures {}; drawn {:.0f}/frame (pulled {:.0f}), missing vs "
      "{:.0f} ps {:.0f}, "
      "skipped {:.0f}, new pipelines {}; geometry: hits {:.0f}/frame, uploads {:.1f}/frame ({:.2f} MB/frame), "
      "dynamic {:.0f}/frame, direct copies {:.2f} MB/frame, cached {}; upload buffer full {:.0f}/frame; geo first {:.0f} hot {:.0f} flagged {:.0f} changed {:.1f} /frame, write "
      "callbacks {:.0f}/frame; texture loads {:.1f}/frame ({:.2f} MB/frame)",
      seconds, stats_.swaps / seconds, stats_.draws / frames, stats_.indices / frames, stats_.copies / frames,
      stats_.copies_depth / frames, stats_.clears / frames, 100.0 * stats_.front_found / frames,
      stats_.shader_loads / frames, stats_.shader_new, shaders_.size(), render_targets_.size(), resolved_.size(),
      stats_.drawn / frames, stats_.pulled / frames, stats_.missing_vs / frames, stats_.missing_ps / frames, stats_.skipped / frames,
      backend_->pipelines_created(), stats_.geo_hits / frames, stats_.geo_uploads / frames,
      stats_.geo_bytes / frames / 1048576.0, stats_.geo_dynamic / frames, stats_.geo_direct_bytes / frames / 1048576.0,
      geometry_.size(), stats_.upload_full / frames, stats_.geo_first / frames, stats_.geo_hot / frames,
      stats_.geo_flagged / frames, stats_.geo_changed / frames,
      double(write_callbacks_.load() - write_callbacks_reported_) / frames, stats_.tex_loads / frames,
      stats_.tex_bytes / frames / 1048576.0);
  write_callbacks_reported_ = write_callbacks_.load();
  if (stats_.prof_draws) {
    // SdnxTicks -> microseconds per drawn draw and per frame (§16.141).
    const double to_us = double(SdnxTicksToNs(1000000000ull)) / 1e12;
    const double d = double(stats_.prof_draws);
    double total = 0;
    for (uint64_t ns : stats_.prof_ns) total += double(ns);
    {
      // The backend counts the draws whose constants set stayed bound (cumulative): this report's share.
      static uint64_t reported = 0;
      const uint64_t now = backend_->constants_bind_skipped();
      stats_.constants_bind_skipped = now - reported;
      reported = now;
    }
    REXGPU_INFO("[sdnx_prof] per draw (us): state {:.2f} vertices {:.2f} (pairing {:.2f} pulled {:.2f} bindings {:.2f}) indices {:.2f} constants+textures {:.2f} "
                "pipeline {:.2f} record {:.2f} | total {:.2f} us/draw, {:.2f} ms/frame in draws | hashed geometry: "
                "hits {:.0f} (reused {:.0f}) fills {:.0f} ({:.2f} MB) /frame, entries {}, hashed {:.1f} MB/frame of bindings {:.1f} MB; hot refusals {:.0f}/frame, page age {:.0f} frames, strikes avg {:.1f} max {}; constants reused {:.0f}/frame, set binds skipped {:.0f}/frame, texture memo hits {:.0f}/frame (kept over resolves {:.0f}, from earlier frames {:.0f}), shader loads without hash {:.0f}/frame, full barriers {:.0f}/frame (attachment {:.0f}, none {:.0f}), textures evicted {}, textures {}",
                stats_.prof_ns[0] * to_us / d, stats_.prof_ns[1] * to_us / d, stats_.prof_sub[0] * to_us / d,
                stats_.prof_sub[1] * to_us / d, stats_.prof_sub[2] * to_us / d, stats_.prof_ns[2] * to_us / d,
                stats_.prof_ns[3] * to_us / d, stats_.prof_ns[4] * to_us / d, stats_.prof_ns[5] * to_us / d,
                total * to_us / d, total * to_us / 1000.0 / frames, stats_.hash_hits / frames, stats_.hash_reuse / frames,
                stats_.hash_fills / frames, stats_.hash_fill_bytes / frames / 1048576.0, hashed_.size(),
                stats_.hash_bytes / frames / 1048576.0, stats_.hash_whole_bytes / frames / 1048576.0, stats_.hot_checks / frames,
                stats_.hot_checks ? double(stats_.hot_age) / stats_.hot_checks : 0.0,
                stats_.hot_checks ? double(stats_.hot_strikes) / stats_.hot_checks : 0.0, stats_.hot_strikes_max,
                stats_.constants_reused / frames, stats_.constants_bind_skipped / frames, stats_.tex_memo_hits / frames, stats_.tex_memo_kept / frames, stats_.tex_memo_carried / frames,
                stats_.shader_load_hits / frames, stats_.full_barriers / frames, stats_.attachment_barriers / frames,
                stats_.pass_barriers_skipped / frames, stats_.textures_evicted, textures_.size());
  }
  if (frames > 0) {  // §16.172
    const uint64_t predicated = predicated_draws_skipped_;
    REXGPU_INFO("[sdnx_tiles] per frame: first-tile draws drawn whole {:.0f}, later-tile draws skipped {:.0f}, "
                "resolves moved {:.0f}, clears deferred {:.0f}; draws dropped by the bin predicate {:.0f}, drawn "
                "anyway in the whole first tile {:.0f}",
                stats_.tile_expanded / frames, stats_.tile_skipped / frames, stats_.tile_resolves_moved / frames,
                stats_.tile_clears_skipped / frames, double(predicated - predicated_reported_) / frames,
                double(predicated_draws_forced_ - forced_reported_) / frames);
    predicated_reported_ = predicated;
    forced_reported_ = predicated_draws_forced_;
  }
  if (frames > 0) {  // §16.171
    REXGPU_INFO("[sdnx_const] per frame: VS {:.0f} uploads {:.0f} KB, PS {:.0f} uploads {:.0f} KB, shared {:.0f} uploads "
                "{:.0f} KB; not copied (unread registers, 16.181) {:.0f} KB", stats_.const_uploads[0] / frames,
                stats_.const_bytes[0] / frames / 1024.0, stats_.const_uploads[1] / frames,
                stats_.const_bytes[1] / frames / 1024.0, stats_.const_uploads[2] / frames,
                stats_.const_bytes[2] / frames / 1024.0, stats_.const_sparse_saved / frames / 1024.0);
  }
  if (!barrier_sites_.empty()) {
    std::vector<std::pair<uint64_t, std::string>> sites;
    for (const auto& it : barrier_sites_) sites.push_back({it.second, it.first});
    std::sort(sites.rbegin(), sites.rend());
    std::string text;
    for (size_t i = 0; i < sites.size() && i < 10; ++i) {
      text += fmt::format(" {} {:.0f}", sites[i].second, double(sites[i].first) / frames);
    }
    REXGPU_INFO("[sdnx_prof] full barriers per frame by site:{}", text);
    barrier_sites_.clear();
  }
  stats_ = {};
  stats_start_ = now;
}

// SDNX_LUMA_LOG=1 (bench diagnostic): the mean brightness of every presented frame, read back three frames later
// (when its slot's fence has been waited for); a frame much brighter than the ones before is logged. Made to catch
// the single white scene frames seen on the console (HUD drawn over a white 3D image, 2026-11-13).
void NativeCommandProcessor::LumaRecord(const Image& front) {
  static const bool enabled = std::getenv("SDNX_LUMA_LOG") != nullptr;
  if (!enabled || front.format != VK_FORMAT_R8G8B8A8_UNORM) return;
  uint32_t slot = uint32_t(frame_ % kFramesInFlight);
  LumaSlot& s = luma_slots_[slot];
  VkDeviceSize size = VkDeviceSize(front.width) * front.height * 4;
  if (!s.buffer || s.size < size) {
    if (s.buffer) return;  // size changed: skip (rare)
    s.buffer = backend_->CreateBuffer(size, BufferUsage::kReadback, &s.mapping);
    if (!s.buffer) return;
    s.size = size;
  }
  EndRendering();
  FullBarrier();
  ImageRegion region;
  region.width = front.width;
  region.height = front.height;
  backend_->CopyImageToBuffer(front.gpu, region, Aspect::kColor, {s.buffer, 0}, 0);
  transfer_pending_ = true;  // see BeginPass: the next rendering pass needs a full barrier
  s.pending = true;
  s.width = front.width;
  s.height = front.height;
  s.swap = stats_total_swaps_;
}

void NativeCommandProcessor::LumaCheck(uint32_t slot) {
  LumaSlot& s = luma_slots_[slot];
  if (!s.pending || !s.mapping) return;
  s.pending = false;
  const uint8_t* p = s.mapping;
  uint64_t sum = 0, count = 0;
  for (uint32_t y = 0; y < s.height; y += 8) {
    const uint8_t* row = p + size_t(y) * s.width * 4;
    for (uint32_t x = 0; x < s.width; x += 8, ++count) sum += row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2];
  }
  double mean = count ? double(sum) / double(count * 3) : 0.0;
  double average = luma_history_count_ ? luma_history_sum_ / luma_history_count_ : mean;
  if (luma_history_count_ >= 8 && mean - average > 60.0) {
    REXGPU_WARN("[sdnx_luma] bright frame: swap {} mean {:.1f} (recent average {:.1f})", s.swap, mean, average);
  }
  // Running average of the last ~30 frames (exponential).
  if (luma_history_count_ < 30) {
    luma_history_sum_ += mean;
    ++luma_history_count_;
  } else {
    luma_history_sum_ += mean - luma_history_sum_ / luma_history_count_;
  }
}

void NativeCommandProcessor::BeginFrontDump(const Image& front) {
  uint32_t bpp = (front.format == VK_FORMAT_R16G16B16A16_UNORM || front.format == VK_FORMAT_R16G16B16A16_SFLOAT)
                     ? 8u
                     : 4u;
  VkDeviceSize size = VkDeviceSize(front.width) * front.height * bpp;
  if (!dump_buffer_ || dump_size_ < size) {
    if (dump_buffer_) {
      backend_->WaitIdle();  // the last dump's copy may still be pending
      backend_->DestroyBuffer(dump_buffer_);
      dump_buffer_ = nullptr;
      dump_mapping_ = nullptr;
    }
    dump_buffer_ = backend_->CreateBuffer(size, BufferUsage::kReadback, &dump_mapping_);
    if (!dump_buffer_) {
      REXGPU_ERROR("[sdnx_front] could not create the readback buffer");
      dump_remaining_ = 0;
      return;
    }
    dump_size_ = size;
  }
  dump_address_ = front_address_;
  dump_width_ = front.width;
  dump_height_ = front.height;
  dump_bpp_ = bpp;
  dump_float_ = front.format == VK_FORMAT_R32_SFLOAT;
  FullBarrier();
  ImageRegion region;
  region.width = front.width;
  region.height = front.height;
  backend_->CopyImageToBuffer(front.gpu, region, Aspect::kColor, {dump_buffer_, 0}, 0);
  transfer_pending_ = true;  // see BeginPass: the next rendering pass needs a full barrier
  dump_pending_ = true;
}

void NativeCommandProcessor::DumpResolveIfRequested(uint32_t dest_base, const Image& dest) {
  static const uint32_t dump_addr =
      std::getenv("SDNX_DUMP_ADDR") ? uint32_t(std::strtoul(std::getenv("SDNX_DUMP_ADDR"), nullptr, 16)) : 0;
  static const uint32_t nth = std::getenv("SDNX_DUMP_NTH") ? uint32_t(std::atoi(std::getenv("SDNX_DUMP_NTH"))) : 0;
  if (!dump_nth_frame_ || !nth || dest_base != dump_addr || ++dump_nth_count_ != nth) return;
  dump_nth_frame_ = false;
  if (dump_pending_ || dump_remaining_ <= 0) return;
  front_address_ = dest_base;
  BeginFrontDump(dest);
}

void NativeCommandProcessor::FinishFrontDump() {
  if (!dump_pending_) return;
  dump_pending_ = false;
  backend_->WaitIdle();
  const uint8_t* data = dump_mapping_;
  if (!data) return;
  double sum[3] = {0, 0, 0};
  double maxv[3] = {0, 0, 0};
  float float_min = FLT_MAX, float_max = -FLT_MAX;
  uint64_t nonblack = 0;
  uint64_t count = uint64_t(dump_width_) * dump_height_;
  std::string ppm;
  // The image of the last requested swap is written (the first one is usually a boot screen).
  const bool write_ppm = dump_remaining_ == 1;
  if (write_ppm) ppm.reserve(size_t(count) * 3 + 64);
  for (uint64_t i = 0; i < count; ++i) {
    float rgb[3] = {0, 0, 0};
    static const bool dump_alpha = std::getenv("SDNX_DUMP_ALPHA") != nullptr;  // alpha as grey
    if (dump_float_) {
      float f;
      std::memcpy(&f, data + i * 4, 4);
      float_min = std::min(float_min, f);
      float_max = std::max(float_max, f);
      rgb[0] = rgb[1] = rgb[2] = f;
    } else if (dump_bpp_ == 4) {
      uint32_t v = *reinterpret_cast<const uint32_t*>(data + i * 4);
      rgb[0] = float(v & 0xFF) / 255.0f;
      rgb[1] = float((v >> 8) & 0xFF) / 255.0f;
      rgb[2] = float((v >> 16) & 0xFF) / 255.0f;
      if (dump_alpha) rgb[0] = rgb[1] = rgb[2] = float(v >> 24) / 255.0f;
    } else {
      const uint16_t* h = reinterpret_cast<const uint16_t*>(data + i * 8);
      auto half_to_float = [](uint16_t hh) {
        uint32_t sign = (hh >> 15) & 1, exp = (hh >> 10) & 0x1F, frac = hh & 0x3FF;
        float v;
        if (exp == 0) {
          v = frac ? std::ldexp(float(frac) / 1024.0f, -14) : 0.0f;
        } else if (exp == 31) {
          v = frac ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
        } else {
          v = std::ldexp(1.0f + float(frac) / 1024.0f, int(exp) - 15);
        }
        return sign ? -v : v;
      };
      rgb[0] = half_to_float(h[0]);
      rgb[1] = half_to_float(h[1]);
      rgb[2] = half_to_float(h[2]);
    }
    for (int c = 0; c < 3; ++c) {
      sum[c] += rgb[c];
      if (rgb[c] > maxv[c]) maxv[c] = rgb[c];
    }
    if (rgb[0] + rgb[1] + rgb[2] > 0.002f) ++nonblack;
    if (write_ppm) {
      // Through an unsigned integer: char(255.0f) overflows a signed char (undefined, it wrote zeros).
      for (int c = 0; c < 3; ++c) {
        float v = std::min(1.0f, std::max(0.0f, rgb[c]));
        ppm.push_back(char(static_cast<unsigned char>(uint32_t(v * 255.0f + 0.5f))));
      }
    }
  }
  double avg[3] = {sum[0] / count, sum[1] / count, sum[2] / count};
  REXGPU_INFO("[sdnx_front] addr={:#x} {}x{}: avg=({:.4f},{:.4f},{:.4f}) max=({:.3f},{:.3f},{:.3f}) nonblack={:.1f}%",
              dump_address_,
              dump_width_, dump_height_, avg[0], avg[1], avg[2], maxv[0], maxv[1], maxv[2],
              100.0 * double(nonblack) / double(count));
  if (dump_float_) REXGPU_INFO("[sdnx_front] R32F range {:.6f} .. {:.6f}", float_min, float_max);
  if (write_ppm && !ppm.empty()) {
    std::filesystem::path path = rex::filesystem::GetExecutableFolder() / "front_dump.ppm";
    if (std::FILE* f = std::fopen(path.string().c_str(), "wb")) {
      std::fprintf(f, "P6\n%u %u\n255\n", dump_width_, dump_height_);
      std::fwrite(ppm.data(), 1, ppm.size(), f);
      std::fclose(f);
      REXGPU_INFO("[sdnx_front] wrote {}", path.string());
    }
  }
  ++dump_index_;
  --dump_remaining_;
}

#if REX_PLATFORM_SWITCH
// Guest presents for the Switch profiler (switch_perf.cpp): the "game N fps" of rex_profile.log and the FPS /
// heartbeat of the console overlays (Status Monitor, Ultrahand) through SaltyNX. The Vulkan backend counts them in
// its IssueSwap; without this the overlays showed nothing for the native renderer.
extern "C" void RexSwitchPerfCount(unsigned id);
#endif

void NativeCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
  (void)frontbuffer_ptr;
  ++stats_.swaps;
  ++stats_total_swaps_;
  // §16.132: this thread's time per frame: waiting for the game, translating draws and resolves, the rest (registers,
  // packets, swap); GPU fence waits happen inside those.
  {
    const uint64_t now = SdnxNowNs();
    if (!cpu_window_start_ns_) {
      cpu_window_start_ns_ = now;
      cpu_gpu_wait_base_ns_ = backend_->CpuWaitNs();
      cpu_idle_ns_ = cpu_draw_ns_ = cpu_copy_ns_ = cpu_window_draws_ = 0;
    } else if (stats_total_swaps_ % 30 == 0) {
      const double frames = 30.0, ms = 1e6;
      const uint64_t gpu_wait = backend_->CpuWaitNs() - cpu_gpu_wait_base_ns_;
      const double wall = double(now - cpu_window_start_ns_) / frames / ms;
      const double idle = double(cpu_idle_ns_) / frames / ms, draw = double(cpu_draw_ns_) / frames / ms;
      const double copy = double(cpu_copy_ns_) / frames / ms, gw = double(gpu_wait) / frames / ms;
      const double per_draw = cpu_window_draws_ ? double(cpu_draw_ns_) / 1e3 / double(cpu_window_draws_) : 0.0;
      REXGPU_INFO("[sdnx_cpu] {:.1f} ms/frame: waiting for the game {:.1f}, draws {:.1f} ({} a frame, {:.1f} us each), "
                  "resolves {:.1f}, other {:.1f}; GPU fence waits {:.1f} (inside the above)",
                  wall, idle, draw, cpu_window_draws_ / 30, per_draw, copy, wall - idle - draw - copy, gw);
      REXGPU_INFO("[sdnx_query] 30 frames: {} queries ended, {} answered visible ({} without a count yet); draws inside "
                  "them {}: no depth attachment {}, depth test off {}, ZFUNC always {}, outside a pass {}",
                  qdiag_queries_, qdiag_visible_, qdiag_first_, qdiag_draws_, qdiag_no_depth_, qdiag_no_ztest_,
                  qdiag_always_, qdiag_no_pass_);
      qdiag_draws_ = qdiag_no_depth_ = qdiag_no_ztest_ = qdiag_always_ = qdiag_no_pass_ = 0;
      qdiag_queries_ = qdiag_visible_ = qdiag_first_ = 0;
      cpu_window_start_ns_ = now;
      cpu_gpu_wait_base_ns_ = backend_->CpuWaitNs();
      cpu_idle_ns_ = cpu_draw_ns_ = cpu_copy_ns_ = cpu_window_draws_ = 0;
    }
  }
#if REX_PLATFORM_SWITCH
  RexSwitchPerfCount(0);
#endif
  {
    static const uint64_t log_frame = std::getenv("SDNX_LOG_FRAME") ? std::strtoull(std::getenv("SDNX_LOG_FRAME"), nullptr, 10) : 0;
    if (log_frame_) REXGPU_INFO("[sdnx_frame] ---- end of swap {} ----", swap_count_);
    ++swap_count_;
    log_frame_ = log_frame && swap_count_ == log_frame;
    // §16.108: a second frame log (with the resolve probes) in case swap 900 was still a loading screen.
    // §16.111: swap 1800 also dumps whole images of the shadow chain (Deko3dBackend::kDumpFrame).
    // §16.117: only with sdnx_deko3d_frame_log (the logs, probes and the ~40 MB dump cost time and SD space).
    static const bool deko3d_frame_log =
        std::strcmp(backend_->name(), "deko3d") == 0 && REXCVAR_GET(sdnx_deko3d_frame_log);
    if (deko3d_frame_log && (swap_count_ == 900 || swap_count_ == 1800)) {
      log_frame_ = true;
    }
    stencil_probes_ = 0;
    // SDNX_LOG_AT / SDNX_DUMP_AT = seconds after the first swap: the frame log / one PPM of that moment (swap
    // numbers do not map to time - loading screens swap far faster than 60 per second).
    static const auto first_swap = std::chrono::steady_clock::now();
    static const double log_at = std::getenv("SDNX_LOG_AT") ? std::atof(std::getenv("SDNX_LOG_AT")) : -1.0;
    static const double dump_at = std::getenv("SDNX_DUMP_AT") ? std::atof(std::getenv("SDNX_DUMP_AT")) : -1.0;
    static bool log_at_done = false, dump_at_done = false;
    double since_first = std::chrono::duration<double>(std::chrono::steady_clock::now() - first_swap).count();
    if (log_at >= 0.0 && !log_at_done && since_first >= log_at) {
      log_at_done = true;
      log_frame_ = true;
    }
    if (dump_at >= 0.0 && !dump_at_done && since_first >= dump_at) {
      dump_at_done = true;
      dump_remaining_ = 1;
    }
#if REX_PLATFORM_SWITCH
    // §16.170: a touch with three fingers or more logs the next frame (every draw: shaders, blending, textures,
    // targets) and writes its picture to front_dump.ppm - to catch an effect at the moment it is on screen. Once per
    // touch, at most 5 times a run.
    {
      static const bool capture_touch = REXCVAR_GET(sdnx_capture_touch);
      static bool capture_armed = true;
      static uint32_t captures = 0;
      HidTouchScreenState touch{};
      const bool touched = capture_touch && hidGetTouchScreenStates(&touch, 1) != 0;
      // §16.175: exactly three fingers held for 8 swaps, with no fourth finger during this touch (fingers land one
      // by one: a four-finger touch passed through three and captured as well).
      static uint32_t three_swaps = 0;
      static bool four_seen = false;
      if (touched && touch.count >= 4) four_seen = true;
      if (!touched || touch.count == 0) four_seen = false;
      three_swaps = touched && touch.count == 3 ? three_swaps + 1 : 0;
      const bool three = three_swaps >= 8 && !four_seen;
      // §16.174: four fingers - the next group of sdnx_skip_cycle (once per touch).
      {
        static bool cycle_armed = true;
        static const bool parsed = [this] {
          std::stringstream groups(REXCVAR_GET(sdnx_skip_cycle));
          for (std::string group; std::getline(groups, group, ',');) {
            std::vector<std::string> parts;
            std::stringstream members(group);
            for (std::string part; std::getline(members, part, '+');) {
              while (!part.empty() && part.front() == ' ') part.erase(part.begin());
              while (!part.empty() && part.back() == ' ') part.pop_back();
              if (!part.empty()) parts.push_back(part);
            }
            if (!parts.empty()) skip_cycle_groups_.push_back(parts);
          }
          return true;
        }();
        (void)parsed;
        const bool four = touched && touch.count >= 4;
        if (four && cycle_armed && !skip_cycle_groups_.empty()) {
          cycle_armed = false;
          skip_cycle_ = (skip_cycle_ + 1) % uint32_t(skip_cycle_groups_.size() + 1);
          std::string names;
          if (skip_cycle_) {
            for (const std::string& part : skip_cycle_groups_[skip_cycle_ - 1]) names += part + " ";
          }
          REXGPU_INFO("[sdnx_skip] four-finger touch: step {} of {}, not drawn: {}", skip_cycle_,
                      skip_cycle_groups_.size(), skip_cycle_ ? names : std::string("(nothing)"));
        }
        if (!touched || touch.count < 4) cycle_armed = true;
      }
      if (three && capture_armed && captures < 5) {
        capture_armed = false;
        ++captures;
        log_frame_ = true;
        if (!dump_pending_) dump_remaining_ = 1;
        backend_->ArmPixelHistory();  // §16.175
        REXGPU_INFO("[sdnx_capture] three-finger touch #{}: frame log of swap {} and front_dump.ppm", captures,
                    swap_count_);
      }
      if (!three) capture_armed = true;
    }
#endif
    log_draw_ = 0;
    if (log_frame_) REXGPU_INFO("[sdnx_frame] ---- swap {} ----", swap_count_);
  }
  ReportStats();
  EnsureFrame();
  EndRendering();
  // The front buffer: texture fetch constant 0, which Direct3D points at it before the swap.
  xenos::xe_gpu_texture_fetch_t fetch = register_file_->GetTextureFetch(0);
  uint32_t front_address = (uint32_t(fetch.base_address) << 12) & 0x1FFFFFFF;
  front_address_ = front_address;
  auto front_it = resolved_.find(front_address);
  Image* front = front_it != resolved_.end() ? &front_it->second : nullptr;
  if (front) ++stats_.front_found;

  // Diagnostic: SDNX_DUMP_FRONT=N copies the presented image back for N swaps (statistics + one PPM).
  static bool dump_env_read = false;
  if (!dump_env_read) {
    dump_env_read = true;
    if (const char* env = std::getenv("SDNX_DUMP_FRONT")) {
      dump_remaining_ = std::atoi(env);
      if (dump_remaining_ > 0) {
        REXGPU_INFO("[sdnx_front] dumping the front buffer for {} swaps", dump_remaining_);
      }
    }
  }
  static const bool dump_rt = std::getenv("SDNX_DUMP_RT") != nullptr;
  if (dump_rt && last_color_rt_) {
    front = last_color_rt_;
    front_address_ = 0xFFFFFFF0u;
  }
  // SDNX_DUMP_ADDR=0x...: dump the resolved image of that address instead of the front buffer.
  static const uint32_t dump_addr =
      std::getenv("SDNX_DUMP_ADDR") ? uint32_t(std::strtoul(std::getenv("SDNX_DUMP_ADDR"), nullptr, 16)) : 0;
  if (dump_addr) {
    auto it = resolved_.find(dump_addr);
    if (it != resolved_.end() && it->second.gpu) {
      front = &it->second;
      front_address_ = dump_addr;
    }
  }
  static const bool dump_nth = std::getenv("SDNX_DUMP_NTH") != nullptr;
  if (dump_nth) {
    // The resolve of the next frame is dumped (DumpResolveIfRequested).
    if (dump_remaining_ > 0 && !dump_pending_ && !dump_nth_frame_) {
      dump_nth_frame_ = true;
      dump_nth_count_ = 0;
    }
  } else if (front && dump_remaining_ > 0 && !dump_pending_) {
    BeginFrontDump(*front);
  }

  if (front) LumaRecord(*front);
  SubmitFrame(front, std::max(frontbuffer_width, uint32_t(1)), std::max(frontbuffer_height, uint32_t(1)));
  FinishFrontDump();
}

}  // namespace rex::graphics::sdnx









