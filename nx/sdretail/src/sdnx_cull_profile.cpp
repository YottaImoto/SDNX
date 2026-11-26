// Sleeping Dogs for Nintendo Switch: where the visibility jobs spend their time, per view (SDNX 16.257, a
// measurement only; off unless sdretail_cull_profile).
//
// CullManager_CullBucketTask (sub_8232F3A0) was ~10 % of all game-thread time while driving (16.255). Its task data
// (r5) holds one view: a 4x4 matrix at +0, an occlusion map at +64 (null: none) with a size at +68, the view's bit
// number at +70 (objects whose 16-bit mask at +30 has that bit are skipped as already handled), floats at +76 / +80
// / +88, and the bucket counter at +96; the job walks the buckets of the scene (PersistentCullBucket: 48-byte cull
// infos in, 64-byte cull results out, ~570 guest instructions with ~210 vector operations per object). Here every
// call is timed and counted under its view bit, and the first task data of each view is logged, to know which views
// (camera, shadow cascades, reflections...) the time goes to.
//
// 16.257 measured (driving): view 0 = the camera (occlusion map, +76 = 0.1, +88 = 4201), views 1-3 = three shadow
// cascades (orthographic matrices, no occlusion map, +76 = 0.03 / 0.035 / 0.05, +88 = 1256); ~25 % of a core in all:
// camera 8.5 %, cascades 5 / 5.2 / 6.5 %. Per object the job compares its projected size with +76 (result code 4:
// too small, not drawn; 3: outside the view; 5: occluded by the map, sub_82324248). 16.258:
// sdretail_cull_min_size_camera / _shadow scale +76: fewer small objects drawn (or casting shadows) far away.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/hook.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <string>
#include <unordered_map>

#if defined(__SWITCH__)
#include <switch.h>
#endif

REXCVAR_DEFINE_BOOL(sdretail_cull_profile, false, "Sleeping Dogs",
                    "Time of the visibility jobs (CullManager_CullBucketTask) per view in the log (SDNX 16.257)");
REXCVAR_DEFINE_DOUBLE(sdretail_cull_min_size_camera, 1.0, "Sleeping Dogs",
                      "Minimum screen size of objects drawn by the camera x this (SDNX 16.258; 1.0 = the game's, 0.25-4)");
REXCVAR_DEFINE_DOUBLE(sdretail_cull_min_size_shadow, 1.0, "Sleeping Dogs",
                      "Minimum screen size of shadow casters in the shadow cascades x this (SDNX 16.258; 1.0 = the "
                      "game's, 0.25-4)");

REX_EXTERN(__imp__sub_8232F3A0);

namespace {

uint32_t LoadU32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

float LoadF32(const uint8_t* base, uint32_t address) {
  const uint32_t bits = LoadU32(base, address);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

uint64_t NowNs() {
#if defined(__SWITCH__)
  return armTicksToNs(armGetSystemTick());
#else
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
#endif
}

bool ProfileOn() {
  static const bool on = REXCVAR_GET(sdretail_cull_profile);
  return on;
}

constexpr uint32_t kViews = 33;  // bit numbers 0-31, and 32 for anything else
std::atomic<uint64_t> g_view_ns[kViews], g_view_calls[kViews];
std::atomic<bool> g_view_logged[kViews];
std::atomic<uint64_t> g_calls{0}, g_window_start{0};

void Report(uint64_t now) {
  const uint64_t start = g_window_start.exchange(now, std::memory_order_relaxed);
  if (!start) return;
  const double wall_ms = double(now - start) / 1e6;
  std::string line;
  double total = 0;
  for (uint32_t v = 0; v < kViews; ++v) {
    const uint64_t ns = g_view_ns[v].exchange(0, std::memory_order_relaxed);
    const uint64_t calls = g_view_calls[v].exchange(0, std::memory_order_relaxed);
    if (!calls) continue;
    total += double(ns) / 1e6;
    line += std::format(" | view {}: {} jobs, {:.1f} ms ({:.1f} % of a core)", v, calls, double(ns) / 1e6,
                        100.0 * double(ns) / 1e6 / wall_ms);
  }
  REXLOG_INFO("[sdnx_cull] {:.0f} ms: visibility jobs {:.1f} ms ({:.1f} % of a core){}", wall_ms, total,
              100.0 * total / wall_ms, line);
}

// 16.258: the minimum screen size of a view (+76) x a factor, once per task data. Several jobs of one view share the
// task data (and the game fills it anew every frame): a value equal to the one written here last is not scaled again.
double Factor(double v) { return std::isfinite(v) ? std::clamp(v, 0.25, 4.0) : 1.0; }

double CameraFactor() {
  static const double f = Factor(REXCVAR_GET(sdretail_cull_min_size_camera));
  return f;
}

double ShadowFactor() {
  static const double f = Factor(REXCVAR_GET(sdretail_cull_min_size_shadow));
  return f;
}

bool ScalingOn() { return CameraFactor() != 1.0 || ShadowFactor() != 1.0; }

std::mutex g_scale_mutex;
std::unordered_map<uint32_t, uint32_t> g_scaled;  // task data address -> the value written there (host order)
std::atomic<bool> g_scale_logged[2];

void ScaleMinSize(uint8_t* base, uint32_t task) {
  if (!ScalingOn()) return;
  // views with an occlusion map: the camera; without: the shadow cascades (and other shadow views)
  const bool camera = LoadU32(base, task + 64) != 0;
  const double f = camera ? CameraFactor() : ShadowFactor();
  if (f == 1.0) return;
  std::lock_guard lock(g_scale_mutex);
  const uint32_t bits = LoadU32(base, task + 76);
  auto it = g_scaled.find(task);
  if (it != g_scaled.end() && it->second == bits) return;  // already scaled (and not rewritten by the game since)
  if (g_scaled.size() > 4096) {
    // drop the entries the game has written over since (task data freed or refilled): those are never ours again
    for (auto e = g_scaled.begin(); e != g_scaled.end();) {
      e = LoadU32(base, e->first + 76) != e->second ? g_scaled.erase(e) : std::next(e);
    }
  }
  float value;
  std::memcpy(&value, &bits, 4);
  if (!std::isfinite(value) || value <= 0.0f) return;
  const float scaled = float(double(value) * f);
  uint32_t out;
  std::memcpy(&out, &scaled, 4);
  const uint32_t swapped = __builtin_bswap32(out);
  std::memcpy(base + task + 76, &swapped, 4);
  g_scaled[task] = out;
  if (!g_scale_logged[camera].exchange(true, std::memory_order_relaxed)) {
    REXLOG_INFO("[sdnx_cull] {} view: minimum screen size {:.3f} -> {:.3f} (x{:.2f})", camera ? "camera" : "shadow",
                value, scaled, f);
  }
}

}  // namespace

REX_HOOK_RAW(sub_8232F3A0) {
  if (!ProfileOn() && !ScalingOn()) {
    __imp__sub_8232F3A0(ctx, base);
    return;
  }
  const uint32_t task = ctx.r5.u32;
  ScaleMinSize(base, task);
  if (!ProfileOn()) {
    __imp__sub_8232F3A0(ctx, base);
    return;
  }
  uint32_t bit = (LoadU32(base, task + 68) & 0xFFFF);  // +70: the low half of the word at +68
  if (bit >= 32) bit = 32;
  if (!g_view_logged[bit].exchange(true, std::memory_order_relaxed)) {
    REXLOG_INFO("[sdnx_cull] view {}: task {:#010x}, occlusion map {:#010x} size {}, +72 {:#010x}, +76 {:.3f}, "
                "+80 {:.3f}, +84 {:#010x}, +88 {:.3f}, +92 {:#010x}, r6 {:#010x} r7 {:#010x}; matrix row 0 "
                "{:.3f} {:.3f} {:.3f} {:.3f}, row 3 {:.3f} {:.3f} {:.3f} {:.3f}",
                bit, task, LoadU32(base, task + 64), LoadU32(base, task + 68) >> 16, LoadU32(base, task + 72),
                LoadF32(base, task + 76), LoadF32(base, task + 80), LoadU32(base, task + 84),
                LoadF32(base, task + 88), LoadU32(base, task + 92), ctx.r6.u32, ctx.r7.u32, LoadF32(base, task),
                LoadF32(base, task + 4), LoadF32(base, task + 8), LoadF32(base, task + 12), LoadF32(base, task + 48),
                LoadF32(base, task + 52), LoadF32(base, task + 56), LoadF32(base, task + 60));
  }
  const uint64_t t0 = NowNs();
  __imp__sub_8232F3A0(ctx, base);
  const uint64_t t1 = NowNs();
  g_view_ns[bit].fetch_add(t1 - t0, std::memory_order_relaxed);
  g_view_calls[bit].fetch_add(1, std::memory_order_relaxed);
  const uint64_t calls = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (calls == 1) g_window_start.store(t1, std::memory_order_relaxed);
  if ((calls & 2047) == 0) Report(t1);
}
