/**
 * Sleeping Dogs port (SDNX): the deko3d backend of the native renderer (Switch).
 * See rex/graphics/sdnx/gpu_backend.h and docs/deko3d-backend.md (the mapping specification).
 *
 * Switch only: it is built when REXGLUE_PLATFORM_SWITCH is set, and the frontend picks it in
 * native_graphics_system.cpp instead of the Vulkan provider.
 *
 * What is implemented here (portlog 16.52): the object model, frames, buffers, images, views, descriptor
 * tables, shaders (DKSH), clears, copies, blits, barriers and the render target binding. Draws map the packet
 * state to deko3d calls after docs/deko3d-backend.md; the parts still missing are marked TODO and log once.
 */
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <deko3d.h>

#include <rex/graphics/sdnx/gpu_backend.h>
#include <rex/ui/deko3d/nucleo.h>
#include <sdnx/dk_package.h>

namespace rex::graphics::sdnx {

struct GpuImage {
  DkImage image{};
  rex::deko3d::BloqueImagen allocation;
  DkImageView view{};          // default view: the whole image, its format
  ImageDesc desc;
};

struct GpuBuffer {
  DkMemBlock block = nullptr;
  uint8_t* cpu = nullptr;  // null for kDeviceLocal
  DkGpuAddr gpu = DK_GPU_ADDR_INVALID;
  uint64_t size = 0;
  BufferUsage usage = BufferUsage::kUpload;
  // §16.142: CPU-cached upload buffer: the range written since the last clean (NoteUploadWritten / FlushUploads).
  bool cached = false;
  // §16.143: written ranges in order; contiguous writes (within a cache line) are merged, GPU-only staging between
  // them is left out.
  std::vector<std::pair<uint64_t, uint64_t>> dirty;
};

struct GpuView {
  DkImageView view{};
  uint32_t width = 0, height = 0;   // level 0 of the image (diagnostics)
  VkFormat format = VK_FORMAT_UNDEFINED;
};

struct GpuShader {
  DkShader shader{};
  bool loaded = false;
  std::string name;  // package name (diagnostics)
  std::vector<::sdnx::dk::PackageTexture> textures;
  // §16.129: pixel shaders: the variant without the alpha-test discard (<name>_nat.dksh), bound while it is off.
  DkShader no_alpha{};
  bool has_no_alpha = false;
};

class Deko3dBackend final : public GpuBackend {
 public:
  Deko3dBackend();
  ~Deko3dBackend() override;

  bool Initialize() override;
  void Shutdown() override;
  const char* name() const override { return "deko3d"; }
  const char* ShaderPackage() const override { return "sdnx_dksh"; }
  const char* ShaderExtension() const override { return ".dksh"; }
  // No NDC negation here: DrawPacket mirrors Y with the viewport swizzle (DkSwizzle_NegativeY, deko3d_backend.cpp),
  // which already turns the frontend's downward screen NDC the right way up. Returning true (§16.71) flipped it a
  // second time: the disclaimer came out upside down on the console (§16.73).
  // No NDC negation: the viewport swizzle mirrors Y (§16.73). The NDC variant was tried in §16.80: same result.
  bool ShaderNdcYInverted() const override { return false; }
  float RenderScale() const override { return float(scale_pct_) / 100.0f; }  // §16.123
  uint64_t MaxStorageBufferRange() const override { return 0xFFFFFFFFull; }  // TODO: the NVN limit
  uint64_t pipelines_created() const override { return 0; }                  // deko3d has no pipeline objects

  void BeginFrame(uint32_t slot) override;
  bool SubmitFrame(uint32_t slot) override;
  void WaitIdle() override;

  GpuBuffer* CreateBuffer(uint64_t size, BufferUsage usage, uint8_t** mapping) override;
  void DestroyBuffer(GpuBuffer* buffer) override;
  uint64_t ConstantAlignment() const override { return 256; }

  GpuImage* CreateImage(const ImageDesc& desc) override;
  void DestroyImage(GpuImage* image) override;
  void InitializeImage(GpuImage* image, const ImageDesc& desc, bool clear = true) override;
  GpuView* CreateView(GpuImage* image, const ViewDesc& desc) override;
  void DestroyView(GpuView* view) override;
  bool FormatFilterable(VkFormat format) override;

  void WriteImageDescriptor(ImageType heap, uint32_t index, GpuView* view) override;
  void WriteSamplerDescriptor(uint32_t index, const SamplerDesc& desc) override;

  void ClearColor(GpuImage* image, const ImageRegion& region, const float rgba[4]) override;
  void ClearDepthStencil(GpuImage* image, const ImageRegion& region, Aspect aspects, float depth,
                         uint32_t stencil) override;
  void CopyImage(GpuImage* source, const ImageRegion& from, GpuImage* destination, const ImageRegion& to,
                 Aspect aspect) override;
  void BlitImage(GpuImage* source, const ImageRegion& from, GpuImage* destination, const ImageRegion& to,
                 Aspect aspect, bool linear) override;
  void CopyBufferToImage(BufferRef source, uint32_t row_length, uint32_t image_height, GpuImage* destination,
                         const ImageRegion& to, Aspect aspect) override;
  void CopyImageToBuffer(GpuImage* source, const ImageRegion& from, Aspect aspect, BufferRef destination,
                         uint32_t row_length) override;
  void CopyBuffer(BufferRef source, BufferRef destination, uint64_t size) override;

  void FullBarrier() override;
  void AttachmentBarrier() override;

  bool CreateShader(GpuShader*& shader, bool vertex, const void* code, uint64_t size, const char* name) override;
  void DestroyShader(GpuShader* shader) override;
  void BeginRendering(GpuImage* const colors[4], uint32_t color_count, GpuImage* depth, uint32_t width,
                      uint32_t height) override;
  bool Draw(const DrawPacket& packet) override;
  void EndRendering() override;
  void ClearAttachmentRect(GpuImage* target, const VkRect2D& rect, const float rgba[4]) override;

  bool BeginQuery(uint32_t slot, uint32_t index) override { return false; }  // TODO: dkCmdBufReportCounter
  void EndQuery(uint32_t slot, uint32_t index) override {}
  void ResetQueries(uint32_t slot, uint32_t count) override {}
  void ResolveQueries(uint32_t slot, uint32_t count) override {}
  const uint64_t* QueryResults(uint32_t slot) override { return nullptr; }

  bool Present(uint32_t slot, GpuImage* front, uint32_t front_width, uint32_t front_height, uint32_t width,
               uint32_t height, bool is_8bpc) override;

  uint64_t constants_bind_skipped() const override { return constants_skipped_; }

 private:
  // The frontend allocates up to 8192 descriptors per heap (native_texture.cpp), more than the interface's 4096.
  static constexpr uint32_t kDkHeap = 8192;

  void NotYet(const char* what);
  bool EnsureBuffer(GpuBuffer* buffer, uint64_t size, BufferUsage usage);
  void MakeImageView(GpuImage* image);
  DkResHandle HandleFor(ImageType heap, uint32_t index) const;

  rex::deko3d::Dispositivo device_;
  std::unique_ptr<rex::deko3d::Nucleo> nucleo_;
  rex::deko3d::Presentacion present_;
  bool initialized_ = false;

  // Descriptor tables: the frontend numbers images per heap (2D, 3D, cube); deko3d has one table, so the handle
  // is heap * kHeapSize + index (see docs/deko3d-backend.md).
  std::array<std::vector<uint8_t>, 4> written_images_;  // one flag per handle: descriptor written
  std::array<bool, kHeapSize> written_samplers_{};
  bool image_upload_pending_ = false;
  uint64_t frame_ = 0;  // work slot counter for Nucleo::EmpezarTrabajo
  uint64_t presents_ = 0, draws_ = 0, passes_ = 0;
  // §16.86: full barriers since the last submission; every kBarriersPerSubmit the work is sent in part.
  static constexpr uint32_t kBarriersPerSubmit = 128;
  // §16.90: depth/stencil aspect extraction (raw copy -> 2D engine -> packed copy) through a scratch block.
  static constexpr uint32_t kScratchWidth = 1280, kScratchHeight = 2048;
  DkMemBlock depth_scratch_ = nullptr;
  // §16.92: the 3D pass that extracts depth / stencil (shaders embedded from nx/deko3d/shaders, persistent targets,
  // descriptor slots 3 * kDkHeap .. + kDzSlots past the frontend's heaps).
  static constexpr uint32_t kDzSlots = 64;
  bool dz_ready_ = false, dz_failed_ = false;
  DkShader dz_vsh_{}, dz_depth_fsh_{}, dz_stencil_fsh_{};
  DkShader dz_restore_depth_fsh_{}, dz_restore_stencil_fsh_{};  // §16.106
  // §16.113: the extraction reads raw 8-byte ZF32_X24S8 texels copied by the copy engine into this scratch (as an
  // R32_UINT buffer texture) instead of sampling the depth through the texture unit.
  DkShader dz_raw_depth_fsh_{}, dz_raw_stencil_fsh_{};
  GpuBuffer raw_scratch_{};
  // §16.120: one extraction pass writes depth and stencil; the scratch targets stay valid for the other aspect of the
  // same region until anything may write a depth/stencil image (ds_epoch_ is bumped then).
  DkShader dz_raw_both_fsh_{};
  DkShader dz_cas_fsh_{};  // §16.148: sharpening on the way to the screen
  int32_t sharpen_ = 0;    // 0..100, resolved from sdnx_sharpen at Initialize
  bool cas_sampler_written_ = false;
  static constexpr uint32_t kCasImageSlot = 0;  // first descriptor of the dz slots (3 * kDkHeap + 0)
  // §16.149: the last sampler (4095): a texture handle has 12 bits for the sampler (4096 overflowed into the image
  // id and faulted the GPU on the first present); the frontend hands out samplers 1..kHeapSize-2 only.
  static constexpr uint32_t kCasSampler = kHeapSize - 1;
  bool PresentSharpened(DkCmdBuf cmd, GpuImage* front, uint32_t src_w, uint32_t src_h, const DkImageView& target);
  bool extract_valid_ = false;
  GpuImage* extract_source_ = nullptr;
  ImageRegion extract_region_{};
  uint64_t extract_epoch_ = 0, ds_epoch_ = 0;
  uint64_t depth_restores_ = 0, stencil_restores_ = 0;
  bool RestoreDepthStencil(BufferRef source, uint32_t row_length, GpuImage* destination, const ImageRegion& to,
                           Aspect aspect);
  DkImage dz_r32f_{}, dz_r8_{};
  DkImageView dz_r32f_view_{}, dz_r8_view_{};
  uint32_t dz_next_slot_ = 0;
  DkGpuAddr breadcrumb_gpu_ = 0;  // §16.94: the GPU writes each finished draw's number here
  // §16.123: internal render scale. Images with ImageDesc::scaled are allocated at scale_pct_ % of their guest size;
  // every region, viewport and scissor the frontend gives in guest pixels for them is mapped here (Phys*).
  uint32_t scale_pct_ = 100;
  bool pass_scaled_ = false;
  uint32_t Phys(uint32_t v) const { return uint32_t((uint64_t(v) * scale_pct_ + 50) / 100); }
  ImageRegion PhysRegion(const GpuImage* image, const ImageRegion& r) const {
    if (!image || !image->desc.scaled || scale_pct_ == 100) return r;
    ImageRegion p = r;
    const uint32_t x0 = Phys(uint32_t(std::max(r.x, 0))), y0 = Phys(uint32_t(std::max(r.y, 0)));
    const uint32_t x1 = Phys(uint32_t(std::max(r.x, 0)) + r.width), y1 = Phys(uint32_t(std::max(r.y, 0)) + r.height);
    p.x = int32_t(x0);
    p.y = int32_t(y0);
    p.width = r.width ? std::max(x1 - x0, 1u) : 0;
    p.height = r.height ? std::max(y1 - y0, 1u) : 0;
    return p;
  }
  // §16.118: two timestamp reports per work slot ({value, timestamp} at s*32 and s*32+16), summed in BeginFrame.
  DkMemBlock gpu_time_block_ = nullptr;
  uint64_t gpu_time_ns_ = 0, gpu_time_frames_ = 0;
  // §16.119: GPU profile of one frame in kProfEvery: a timestamp at the start of every backend operation; the time to
  // the next mark is charged to this mark's label (passes by target shape). Printed as [sdnx_prof] after the frame.
  static constexpr uint32_t kProfMarks = 4096, kProfEvery = 300;
  DkMemBlock prof_block_ = nullptr;
  DkMemBlock const_block_ = nullptr;  // §16.139
  uint64_t cleaned_reported_ = 0;     // §16.141
  std::vector<GpuBuffer*> cached_uploads_;  // §16.142
  uint64_t upload_cleaned_bytes_ = 0;
  DkGpuAddr const_addr_ = 0;
  bool prof_active_ = false;
  uint32_t prof_count_ = 0;
  std::vector<std::string> prof_labels_;
  std::vector<uint64_t> prof_draws_;
  // §16.130: pixel-shader choice of the profiled frame's draws: [0] early Z variant, [1] alpha test off but no variant,
  // [2 + func] alpha test on with that Xenos function.
  std::array<uint32_t, 10> prof_kind_{};
  std::vector<std::array<uint32_t, 10>> prof_kinds_;
  void Mark(std::string label);
  void ReportProfile();
  bool EnsureDz();
  // §16.98: one-time on-console test of storage buffer 3 as read by a pixel and by a vertex shader.
  static constexpr uint64_t kSelfTestFrame = 600;  // about 20 s in (the test does not depend on what the game shows)
  bool selftest_loaded_ = false;
  DkShader st_vsh_{}, st_pass_fsh_{}, st_read_fsh_{}, st_descriptor_fsh_{}, st_ldchi_fsh_{}, st_movread_fsh_{};
  // §16.105: pulled vertices are read through an R32_UINT buffer texture over the whole upload buffer (vertex texture
  // binding 31, shader_common.h g_PullTexels), one descriptor per upload buffer at 3 * kDkHeap + kDzSlots + n.
  static constexpr uint32_t kPullSlots = 8;
  static constexpr uint32_t kPullBinding = 31;
  struct PullTexture { const GpuBuffer* buffer = nullptr; uint32_t handle = 0; };
  PullTexture pull_textures_[kPullSlots];
  uint32_t pull_texture_count_ = 0;
  bool PullTextureHandle(const GpuBuffer* buffer, uint32_t& handle);
  uint32_t selftest_kind_ = 0;  // §16.103: 0 descriptor, 1 LDC.64 high word, 2 load through MOV address, 3 LDC.64 load
  static constexpr uint32_t kSelfTests = 6;
  GpuBuffer* selftest_buffer_ = nullptr;
  DkMemBlock st_uncached_ = nullptr, st_image_ = nullptr;
  bool selftest_pending_ = false;
  uint32_t selftest_source_ = 0, selftest_expected_ = 0;
  DkGpuAddr selftest_address_ = 0;
  bool selftest_descriptors_ok_ = true;
  const char* selftest_name_ = "";
  void RecordStorageSelfTest(uint32_t source);
  void ReportStorageSelfTest();
  bool ExtractDepthStencil(GpuImage* source, const ImageRegion& from, Aspect aspect, BufferRef destination,
                           uint32_t row_length);
  uint32_t barriers_since_submit_ = 0;
  uint64_t partial_submits_ = 0;
  uint32_t pass_draws_ = 0;
  uint32_t draws_of_frame_ = 0;  // reset in BeginFrame; the probe frame logs the first draws
  bool pass_final_ = false;
  GpuImage* scene_target_ = nullptr;  // last colour target that is not the screen size (the scene the final pass reads)
  GpuImage* current_pass_target_ = nullptr;  // single colour target of the open pass
  GpuImage* probe_snapshot_ = nullptr;        // the scene copied right after its pass (probe frames)
  bool probe_snapshot_valid_ = false;
  bool pass_has_depth_ = false;  // the open pass has a depth target (Vulkan ignores a depth test without one, deko3d does not)
  std::unordered_map<const GpuImage*, uint32_t> draws_since_clear_;  // per target: draws since its last clear
  uint32_t probe_scene_bpp_ = 4;
  GpuImage* probe_snapshot_last_ = nullptr;  // the scene after the LAST scene pass of the probe frame
  // Probe frame: what the textures of the final pass contain (§16.73). Descriptor -> the view written there.
  struct DescriptorInfo { DkImageView view{}; uint32_t width = 0, height = 0; VkFormat format = VK_FORMAT_UNDEFINED; };
  std::vector<DescriptorInfo> descriptor_info_;  // 3 * kDkHeap, filled by WriteImageDescriptor
  rex::deko3d::Subida final_tex_samples_{};
  // §16.108: DebugProbe (the frontend calls it after each resolve of its frame log): 9 blocks of 8x8 texels per
  // probe, statistics per channel logged after the frame ([sdnx_probe]).
  void DebugProbe(GpuImage* image, const ImageRegion& region, uint32_t tag, const char* kind) override;
  void ReportDebugProbes();
  struct DebugProbeEntry {
    uint32_t tag; const char* kind; VkFormat format; uint32_t bpp, x, y, width, height; bool stencil;
  };
  std::vector<DebugProbeEntry> debug_probes_;
  rex::deko3d::Subida debug_probe_samples_{};
  static constexpr uint32_t kMaxDebugProbes = 96;
  void CopyProbeBlocks(const DkImageView& view, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
  // §16.111: whole images of the shadow chain on one frame, written to sdmc:/switch/sdretail/dumps after it.
  static constexpr uint64_t kDumpFrame = 1800;
  static constexpr size_t kDumpBytes = size_t(64) << 20;
  struct DebugDump { std::string name; VkFormat format; uint32_t bpp, width, height; size_t offset; };
  std::vector<DebugDump> debug_dumps_;
  DkMemBlock dump_block_ = nullptr;
  size_t dump_used_ = 0;
  uint32_t dump_before_equal_ = 0;
  bool DumpWanted(uint32_t tag, const char* kind);
  void QueueDump(const DkImageView& view, uint32_t x, uint32_t y, uint32_t width, uint32_t height, VkFormat format,
                 uint32_t bpp, const std::string& name);
  void WriteDumps();
  DkImageView* RunExtractionPass(GpuImage* source, const ImageRegion& from, bool depth);
  struct FinalTexProbe { uint32_t image, width, height, bpp; VkFormat format; const char* what; };
  std::vector<FinalTexProbe> final_tex_probes_;
  static constexpr uint32_t kMaxFinalProbes = 6;
  // §16.76: pipeline statistics around the final-pass draws of the probe frame.
  static constexpr uint32_t kFinalCounters = 6, kMaxFinalCounts = 6;
  rex::deko3d::Subida final_count_buffer_{};
  uint32_t final_counts_ = 0;
  uint32_t final_count_draws_[kMaxFinalCounts] = {};
  // §16.77: per-frame pipeline statistics on sampled frames: the whole frame (entry 0) and each final pass (1..8).
  static constexpr uint32_t kStatCounters = 4, kStatEntries = 81;
  rex::deko3d::Subida stats_buffer_{};
  uint32_t stats_passes_ = 0;            // passes recorded in this frame (final ones; every pass on AllPassFrame)
  uint64_t stats_pass_ids_[kStatEntries] = {};
  uint32_t stats_pass_draws_[kStatEntries] = {};
  struct PassInfo { uint32_t w, h, colors; int fmt; bool depth, final_pass; float vp[4]; };
  PassInfo stats_pass_info_[kStatEntries] = {};
  bool stats_first_draw_pending_ = false;
  bool AllPassFrame() const { return presents_ == 300 || presents_ == 700; }
  bool stats_pass_open_ = false;
  bool StatsFrame() const { return presents_ < 700 ? presents_ % 10 == 0 : presents_ % 100 == 0; }
  void ReportStats(uint32_t entry, uint32_t side);
  // Records copies of 3x3 blocks of 8x8 texels of `view` (w x h from its origin) for Present to print.
  void ProbeRegion(const DkImageView& view, uint32_t w, uint32_t h, uint32_t bpp, VkFormat format, uint32_t id,
                   const char* what);
  bool scene_snapshot_taken_ = false;       // the first snapshot of this probe frame is already stored  // bytes per pixel of the scene snapshot (from its format)  // the open pass targets the screen size (the final composite)  // draws in the open pass (the first one is logged with its viewport/scissor)  // heartbeat counters (see Present)
  // The DKSH package: index.txt (names, keys) and textures.txt (each shader's texture bindings by Xenos slot).
  // CreateShader copies a shader's bindings into its GpuShader.
  ::sdnx::dk::Package package_;
  std::string package_dir_;
  std::unordered_set<std::string> nat_names_;  // §16.129: nat.txt, the shaders with a _nat variant
  bool early_z_ = true;
  // §16.131: ZCULL on (sdnx_zcull); its data is stale after a depth/stencil write outside the 3D engine's draws and
  // clears (copies, blits, the restore passes, a new image at a reused address): the next pass with depth drops it.
  bool zcull_ = false;
  bool invalidate_zcull_ = true;
  uint64_t zcull_invalidations_ = 0;
  // §16.132: ZCULL keeps one test direction: draws testing depth the other way (GREATER, ALWAYS with writes...) or
  // testing stencil drop it first; zcull_inert_ = already dropped since the last draw that could rebuild it.
  bool zcull_inert_ = false;
  uint64_t zcull_draw_invalidations_ = 0;

 public:
  uint64_t CpuWaitNs() const override { return nucleo_ ? nucleo_->NsEsperandoValla() : 0; }
  // §16.139: the persistent VS / PS constant blocks (const_block_: VS at 0, PS at 4096).
  bool PushConstantsSupported() const override { return const_addr_ != 0; }
  void NoteUploadWritten(GpuBuffer* buffer, uint64_t begin, uint64_t end) override;
  // §16.142: cleans the written ranges of the cached upload buffers; before every submit.
  void FlushUploads();
  void PushConstants(uint32_t block, uint32_t offset, const void* data, uint32_t size) override;

 private:
  uint64_t nat_draws_ = 0;  // draws that used the variant (heartbeat)

  // Draw state already recorded in the open command buffer (deko3d keeps it until it changes).
  struct BoundState {
    DkShader const* vs = nullptr;
    DkShader const* ps = nullptr;
    DkRasterizerState raster{};
    DkColorState color{};
    DkColorWriteState color_write{};
    DkBlendState blend[4]{};
    DkDepthStencilState depth{};
    DkViewport viewport{};
    bool viewport_set = false;
    DkScissor scissor{};
    bool scissor_set = false;
    bool valid = false;
    // §16.153: the rest of the per-draw state, so Draw records only what changed since the previous draw.
    bool shaders_set = false, raster_set = false, color_set = false, depth_set = false, swizzle_set = false;
    bool blend_set[4]{};
    uint8_t stencil[6]{};
    bool stencil_set = false;
    float blend_const[4]{};
    bool blend_const_set = false;
    float depth_bias[2]{};
    bool depth_bias_set = false;
    int primitive_restart = -1;
    DkVtxAttribState attribs[32]{};
    DkVtxBufferState vtx_buffer_state[16]{};
    uint32_t attrib_count = UINT32_MAX, vtx_buffer_state_count = UINT32_MAX;
    DkBufExtents vtx_buffers[16]{};
    bool vtx_buffer_set[16]{};
    DkBufExtents uniforms[4]{};  // vertex 0, fragment 1, vertex 2, fragment 2
    bool uniform_set[4]{};
    DkResHandle textures[2][32]{};  // [0] vertex, [1] fragment, by binding
    bool texture_set[2][32]{};
  } bound_;
  uint64_t state_commands_skipped_ = 0;  // §16.153 (heartbeat)
  uint64_t constants_skipped_ = 0;
  bool rendering_ = false;
  bool logged_not_yet_ = false;
};

std::unique_ptr<GpuBackend> CreateDeko3dBackend();

}  // namespace rex::graphics::sdnx













