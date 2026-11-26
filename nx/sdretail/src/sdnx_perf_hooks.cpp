// Sleeping Dogs for Nintendo Switch: hooks that change only how fast the game runs, never what it does.

#ifdef SDNX_UPSTREAM_SDK
#include <rex/ppc/hook.h>
#else
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#endif

#include <atomic>
#include <cstdint>
#include <thread>

#if defined(__SWITCH__)
#include <switch.h>
#endif

REX_EXTERN(__imp__sub_82BB83F0);

namespace {
std::atomic<uint64_t> g_fence_wait_yields{0};
}

// One step of Direct3D's fence wait (BlockOnFence; called in a loop by sub_82BC01C8 until the GPU passes the fence):
// cctpl, 4 x 8 db16cyc, cctpm, then the checks; r3 = 1 means "still waiting". On Xenon the low thread priority and the
// delays hand the core to the other hardware thread; recompiled they are an empty busy loop - in gameplay the four
// game worker threads spent ~30 % of a core each here (Switch profile 2026-11-13), on three cores shared with the
// GPU command thread and the main thread that they are waiting for. The step now gives its core away while waiting.
REX_HOOK_RAW(sub_82BB83F0) {
  __imp__sub_82BB83F0(ctx, base);
  if (ctx.r3.u32 != 0) {
    g_fence_wait_yields.fetch_add(1, std::memory_order_relaxed);
#if defined(__SWITCH__)
    // A short real sleep, not a bare yield: a yield returns at once when no thread of the same priority is ready on
    // this core, and the GPU thread that has to make progress has a higher priority on another core anyway.
    svcSleepThread(50'000);  // 50 us; the fence advances in steps of a frame part (milliseconds)
#else
    std::this_thread::yield();
#endif
  }
}

uint64_t SdnxFenceWaitYields() { return g_fence_wait_yields.load(std::memory_order_relaxed); }

#ifdef SDNX_UPSTREAM_SDK
// SDNX 16.251: the game's memcpy (sub_82244990, entered through sub_82244490) and memset (sub_82BDBA38) from the XDK
// libraries, done by the host's memcpy / memset. Recompiled they are VMX loops (lvsl/vperm, stvx128, dcbzl, a byte
// tail) that cost ~1.3 % and ~1.2 % of a core in the stack profile of 2026-11-25 (main, worker and Wwise threads). The
// bytes they leave in guest memory are the same: guest memory is the big-endian image itself, both copy byte for
// byte. The original runs instead when the ranges overlap (memcpy leaves that undefined and the original's order
// would matter), when a range crosses into the physical view at 0xE0000000 (its host offset changes there) or when
// the step constants the originals read (0x82F5804C = 1, 0x82F58050 = 16) are not those. sdretail_native_memcpy
// (sdretail.toml, default true): false always runs the originals. The host memcpy and memset use only plain loads and
// stores (LDP/STP, LDR/STR), which the exception handler can emulate on watched pages; zero fills of 160+ bytes go
// through the --wrap=memset of sdnx_memset.cpp (no DC ZVA).
#include <rex/cvar.h>
#include <rex/logging.h>

#include <cstring>

REXCVAR_DEFINE_BOOL(sdretail_native_memcpy, true, "Sleeping Dogs",
                    "The game's memcpy and memset done by the host's (SDNX 16.251); false: the recompiled originals");

REX_EXTERN(__imp__sub_82244990);
REX_EXTERN(__imp__sub_82BDBA38);

namespace {

std::atomic<uint64_t> g_mem_native_calls{0}, g_mem_native_bytes{0}, g_mem_fallbacks{0};

bool NativeMemOn() {
  static const bool on = REXCVAR_GET(sdretail_native_memcpy);
  return on;
}

uint32_t GuestWord(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

// The host address of a guest range, or null when it cannot be one plain host range (the physical view at
// 0xE0000000 sits 0x1000 further in host memory: REX_PHYS_HOST_OFFSET in the generated pch).
uint8_t* HostRange(uint8_t* base, uint32_t address, uint32_t size) {
  const uint64_t end = uint64_t(address) + size;
  if (end > 0x100000000ull) return nullptr;
  if (address < 0xE0000000u && end > 0xE0000000u) return nullptr;
#ifndef REX_PHYSICAL_OFFSET_IN_CODE
#error "REX_PHYSICAL_OFFSET_IN_CODE comes from rex::base (as for the generated code)"
#endif
#if REX_PHYSICAL_OFFSET_IN_CODE
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
#else
  return base + address;
#endif
}

void NoteNative(uint32_t n) {
  const uint64_t calls = g_mem_native_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t bytes = g_mem_native_bytes.fetch_add(n, std::memory_order_relaxed) + n;
  if ((calls & ((1u << 20) - 1)) == 0) {
    REXLOG_INFO("[sdnx_mem] the game's memcpy / memset by the host: {} calls, {:.1f} MB, {} by the originals",
                calls, double(bytes) / 1048576.0, g_mem_fallbacks.load(std::memory_order_relaxed));
  }
}

}  // namespace

REX_HOOK_RAW(sub_82244990) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  if (NativeMemOn() && GuestWord(base, 0x82F5804Cu) == 1) {
    const bool overlap = n && uint64_t(dst) < uint64_t(src) + n && uint64_t(src) < uint64_t(dst) + n;
    uint8_t* to = HostRange(base, dst, n);
    const uint8_t* from = HostRange(base, src, n);
    if (!overlap && to && from) {
      if (n) std::memcpy(to, from, n);
      NoteNative(n);
      return;  // r3 still holds dst, the return value
    }
  }
  g_mem_fallbacks.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82244990(ctx, base);
}

REX_HOOK_RAW(sub_82BDBA38) {
  const uint32_t dst = ctx.r3.u32, n = ctx.r5.u32;
  if (NativeMemOn() && GuestWord(base, 0x82F58050u) == 16) {
    if (uint8_t* to = HostRange(base, dst, n)) {
      if (n) std::memset(to, int(ctx.r4.u32 & 0xFF), n);
      NoteNative(n);
      return;  // r3 still holds dst, the return value
    }
  }
  g_mem_fallbacks.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82BDBA38(ctx, base);
}
#endif  // SDNX_UPSTREAM_SDK
