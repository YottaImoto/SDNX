// Sleeping Dogs for Nintendo Switch: the game's fog volume computed every Nth frame instead of every frame (SDNX
// 16.255). Off at 1.
//
// UpdateFogTask (sub_826F1D68, the job named in the task descriptor at 0x82F38B08) fills the fog volume texture on
// the CPU: 32 slices of 8 KB (32 rows of 16 texels of 8 bytes, row pitch 256 bytes), each texel with vexptefp and
// a dozen vector multiply-adds; it was ~11 % of all game-thread time in gameplay (16.254 profile, the largest single
// function). sub_822B1228 starts it once a frame: it resets the slice counter at 0x82FD7000 and posts three jobs (or
// runs them inline) whose task data (60 bytes) hold the texture's data address (+24, from sub_822C6830 of the
// texture object), the counter's address (+32) and the slice count (+52); the jobs take slices from the shared
// counter. Their only output is the texture.
//
// With sdretail_fog_update_interval = N (> 1), only every Nth start computes. In the other frames the jobs return
// at once, so the texture keeps the previous computation (a fog lag of N-1 frames when the camera turns), and its
// pages are not written (the GPU thread does not reload it either). A frame is skipped only when its texture address
// is the one last computed: a new or moved texture is always computed first.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/hook.h>

#include <atomic>
#include <cstdint>
#include <cstring>

REXCVAR_DEFINE_INT32(sdretail_fog_update_interval, 1, "Sleeping Dogs",
                     "The fog volume (UpdateFogTask) computed every Nth frame (SDNX 16.255; 1 = every frame, 1-4)");

REX_EXTERN(__imp__sub_822B1228);
REX_EXTERN(__imp__sub_826F1D68);

namespace {

uint32_t LoadU32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

int Interval() {
  static const int n = [] {
    const int v = REXCVAR_GET(sdretail_fog_update_interval);
    return v < 1 ? 1 : v > 4 ? 4 : v;
  }();
  return n;
}

std::atomic<uint64_t> g_starts{0};
std::atomic<bool> g_skip{false};           // this frame's start: the jobs skip
std::atomic<uint32_t> g_last_out{0};       // texture address of the last computation
std::atomic<uint64_t> g_jobs_run{0}, g_jobs_skipped{0}, g_out_changes{0};
std::atomic<bool> g_logged_first{false};

}  // namespace

REX_HOOK_RAW(sub_822B1228) {
  if (Interval() > 1) {
    const uint64_t n = g_starts.fetch_add(1, std::memory_order_relaxed) + 1;
    g_skip.store(n % uint64_t(Interval()) != 0, std::memory_order_release);
    if ((n & 4095) == 0) {
      REXLOG_INFO("[sdnx_fog] {} fog updates started: jobs computed {}, skipped {}; texture address changes {}", n,
                  g_jobs_run.load(std::memory_order_relaxed), g_jobs_skipped.load(std::memory_order_relaxed),
                  g_out_changes.load(std::memory_order_relaxed));
    }
  }
  __imp__sub_822B1228(ctx, base);
}

REX_HOOK_RAW(sub_826F1D68) {
  if (Interval() == 1) {
    __imp__sub_826F1D68(ctx, base);
    return;
  }
  const uint32_t task = ctx.r5.u32;
  const uint32_t out = LoadU32(base, task + 24);
  if (!g_logged_first.exchange(true, std::memory_order_relaxed)) {
    REXLOG_INFO("[sdnx_fog] fog volume every {} frames: texture {:#010x}, {} slices, counter {:#010x}", Interval(), out,
                LoadU32(base, task + 52), LoadU32(base, task + 32));
  }
  if (g_skip.load(std::memory_order_acquire) && out != 0 && out == g_last_out.load(std::memory_order_acquire)) {
    g_jobs_skipped.fetch_add(1, std::memory_order_relaxed);
    return;  // the texture keeps the previous computation; the slice counter is reset by the next start
  }
  __imp__sub_826F1D68(ctx, base);
  if (g_last_out.exchange(out, std::memory_order_acq_rel) != out) {
    g_out_changes.fetch_add(1, std::memory_order_relaxed);
  }
  g_jobs_run.fetch_add(1, std::memory_order_relaxed);
}
