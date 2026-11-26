// Sleeping Dogs for Nintendo Switch: the CPU skinning of cars (SkinVehicleBufferTask) skipped when nothing it reads
// has changed since the last time it skinned the same buffer (SDNX 16.256). Off unless sdretail_vehicle_skin_cache.
//
// sub_826E58C8 (the job named SkinVehicleBufferTask in its task descriptor) was ~8 % of all game-thread time while
// driving (16.255), for every car part every frame. Its task data (r5):
//   +0  output vertices (16 bytes each)        +4  input stream, 16 bytes a vertex   +8  input stream, 8 bytes a vertex
//   +12 flags                                  +16 bone sources, 48 bytes each       +20 bone count (u16)
//   +24 input stream, 8 bytes a vertex         +28 a vector (16 bytes)                +32 a count
//   +36 vertex count                           +40 per-bone table (indexed by vertex bytes x 4, read 16 bytes)
//   +52 bone sources, 64 bytes each            +56 bone remap (a byte per bone, 255 = identity)
//   +60 damage decal request                   +72 the car's damage state
// It builds a 64-byte matrix per bone on its stack from +16 / +52 through +56, then skins the +36 vertices of the
// three streams into +0. With a decal request (+60 not 0) it also clips triangles into the car's damage state (+72:
// up to 29 entries, count at +1156, flag at +1152); without one, the output vertices are all it writes.
//
// Here, when there is no decal request: everything the skinning reads (the three streams, the bones used through the
// remap, the per-bone table, the vector, counts and flags) is hashed (XXH3, ~32 bytes a vertex against ~100 vector
// operations of the skinning). If the hash equals the one of the last skinning of the same output buffer (keyed by
// the output and input addresses and the vertex count), the vertices kept from that skinning are copied to the
// output and the original does not run. A parked car, or one waiting at a light, is skinned once.
// sdretail_vehicle_skin_verify = N > 0: every Nth hit runs the original anyway and compares its output with the kept
// one byte for byte (a difference is counted and logged, and the original's output stays).

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/hook.h>

#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

REXCVAR_DEFINE_BOOL(sdretail_vehicle_skin_cache, false, "Sleeping Dogs",
                    "Car skinning (SkinVehicleBufferTask) skipped when its inputs did not change (SDNX 16.256)");
REXCVAR_DEFINE_INT32(sdretail_vehicle_skin_verify, 0, "Sleeping Dogs",
                     "Every Nth car skinning cache hit is recomputed and compared byte for byte (0 = never)");

REX_EXTERN(__imp__sub_826E58C8);

namespace {

constexpr uint32_t kMaxVertices = 25000;
constexpr size_t kCacheBudget = size_t(24) << 20;  // host bytes of kept vertices

uint32_t LoadU32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

uint16_t LoadU16(const uint8_t* base, uint32_t address) {
  uint16_t v;
  std::memcpy(&v, base + address, 2);
  return __builtin_bswap16(v);
}

// The host address of a guest range, or null when it is not one plain host range (see sdnx_perf_hooks.cpp).
uint8_t* HostRange(uint8_t* base, uint32_t address, uint32_t size) {
  const uint64_t end = uint64_t(address) + size;
  if (!address || end > 0x100000000ull) return nullptr;
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

bool CacheOn() {
  static const bool on = REXCVAR_GET(sdretail_vehicle_skin_cache);
  return on;
}

uint32_t VerifyEvery() {
  static const uint32_t n = uint32_t(std::max(0, REXCVAR_GET(sdretail_vehicle_skin_verify)));
  return n;
}

struct Entry {
  uint64_t inputs = 0;
  uint64_t tick = 0;
  std::vector<uint8_t> vertices;
};

std::mutex g_mutex;
std::unordered_map<uint64_t, Entry> g_cache;
size_t g_cache_bytes = 0;
uint64_t g_tick = 0;

std::atomic<uint64_t> g_calls{0}, g_decal{0}, g_unsupported{0}, g_hits{0}, g_misses{0}, g_verified{0},
    g_mismatches{0}, g_evictions{0}, g_hit_vertices{0}, g_miss_vertices{0};

void EvictLocked() {
  if (g_cache_bytes <= kCacheBudget) return;
  std::vector<std::pair<uint64_t, uint64_t>> ages;  // tick, key
  ages.reserve(g_cache.size());
  for (const auto& [key, e] : g_cache) ages.push_back({e.tick, key});
  std::sort(ages.begin(), ages.end());
  for (const auto& [tick, key] : ages) {
    if (g_cache_bytes <= kCacheBudget * 3 / 4) break;
    auto it = g_cache.find(key);
    g_cache_bytes -= it->second.vertices.size();
    g_cache.erase(it);
    g_evictions.fetch_add(1, std::memory_order_relaxed);
  }
}

void MaybeLog(uint64_t calls) {
  if (calls & 8191) return;
  size_t bytes, entries;
  {
    std::lock_guard lock(g_mutex);
    bytes = g_cache_bytes;
    entries = g_cache.size();
  }
  const uint64_t hits = g_hits.load(std::memory_order_relaxed), misses = g_misses.load(std::memory_order_relaxed);
  REXLOG_INFO("[sdnx_skin] {} car skinnings: {} reused ({:.0f} %, {} vertices), {} skinned ({} vertices), {} with a "
              "decal request, {} unsupported; verified {}, differences {}; kept {} buffers, {:.1f} MB, evicted {}",
              calls, hits, hits + misses ? 100.0 * double(hits) / double(hits + misses) : 0.0,
              g_hit_vertices.load(std::memory_order_relaxed), misses, g_miss_vertices.load(std::memory_order_relaxed),
              g_decal.load(std::memory_order_relaxed), g_unsupported.load(std::memory_order_relaxed),
              g_verified.load(std::memory_order_relaxed), g_mismatches.load(std::memory_order_relaxed), entries,
              double(bytes) / 1048576.0, g_evictions.load(std::memory_order_relaxed));
}

// The hash of everything the skinning reads without a decal request; false when a range is not plain host memory.
bool HashInputs(uint8_t* base, uint32_t task, uint32_t vertices, uint64_t* out_hash) {
  const uint32_t bones = LoadU16(base, task + 20);
  if (bones > 255) return false;
  uint8_t header[32];
  std::memcpy(header, base + task + 12, 4);   // flags
  std::memcpy(header + 4, base + task + 20, 4);  // bone count (+ the 2 bytes after it)
  std::memcpy(header + 8, base + task + 28, 16);  // the vector
  std::memcpy(header + 24, base + task + 32, 8);  // count, vertex count
  uint64_t h = XXH3_64bits(header, sizeof(header));
  auto add = [&](uint32_t address, uint32_t size) -> bool {
    if (!size) return true;
    const uint8_t* p = HostRange(base, address, size);
    if (!p) return false;
    h = XXH3_64bits_withSeed(p, size, h);
    return true;
  };
  // bones through the remap: the highest source index used
  uint32_t sources = 0;
  if (bones) {
    const uint8_t* remap = HostRange(base, LoadU32(base, task + 56), bones);
    if (!remap) return false;
    h = XXH3_64bits_withSeed(remap, bones, h);
    for (uint32_t i = 0; i < bones; ++i) {
      if (remap[i] != 255) sources = std::max<uint32_t>(sources, remap[i] + 1u);
    }
  }
  if (!add(LoadU32(base, task + 16), sources * 48) || !add(LoadU32(base, task + 52), sources * 64)) return false;
  // the per-bone table: indexed by vertex bytes x 4, 16 bytes read (the job prefetches bones x bones x 4 bytes)
  const uint32_t table = LoadU32(base, task + 40);
  if (table && !add(table, std::min<uint32_t>(bones * bones * 4 + 12, 4096))) return false;
  if (!add(LoadU32(base, task + 4), vertices * 16) || !add(LoadU32(base, task + 8), vertices * 8) ||
      !add(LoadU32(base, task + 24), vertices * 8)) {
    return false;
  }
  *out_hash = h;
  return true;
}

}  // namespace

REX_HOOK_RAW(sub_826E58C8) {
  if (!CacheOn()) {
    __imp__sub_826E58C8(ctx, base);
    return;
  }
  const uint64_t calls = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  MaybeLog(calls);
  const uint32_t task = ctx.r5.u32;
  const uint32_t out = LoadU32(base, task + 0);
  const uint32_t vertices = LoadU32(base, task + 36);
  if (LoadU32(base, task + 60) != 0) {
    g_decal.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_826E58C8(ctx, base);
    return;
  }
  uint64_t inputs = 0;
  uint8_t* out_host = vertices && vertices <= kMaxVertices ? HostRange(base, out, vertices * 16) : nullptr;
  if (!out_host || !HashInputs(base, task, vertices, &inputs)) {
    g_unsupported.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_826E58C8(ctx, base);
    return;
  }
  const uint64_t key_data[2] = {uint64_t(out) | uint64_t(LoadU32(base, task + 4)) << 32,
                                uint64_t(vertices) | uint64_t(LoadU32(base, task + 8)) << 32};
  const uint64_t key = XXH3_64bits(key_data, sizeof(key_data));
  const size_t bytes = size_t(vertices) * 16;
  bool verify = false;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_cache.find(key);
    if (it != g_cache.end() && it->second.inputs == inputs && it->second.vertices.size() == bytes) {
      const uint64_t hits = g_hits.fetch_add(1, std::memory_order_relaxed) + 1;
      g_hit_vertices.fetch_add(vertices, std::memory_order_relaxed);
      it->second.tick = ++g_tick;
      if (VerifyEvery() && hits % VerifyEvery() == 0) {
        verify = true;
      } else {
        std::memcpy(out_host, it->second.vertices.data(), bytes);
        return;
      }
    }
  }
  __imp__sub_826E58C8(ctx, base);
  std::lock_guard lock(g_mutex);
  Entry& e = g_cache[key];
  if (verify) {
    g_verified.fetch_add(1, std::memory_order_relaxed);
    if (e.vertices.size() == bytes && std::memcmp(e.vertices.data(), out_host, bytes) != 0) {
      const uint64_t n = g_mismatches.fetch_add(1, std::memory_order_relaxed);
      if (n < 8) {
        size_t at = 0;
        while (at < bytes && e.vertices[at] == out_host[at]) ++at;
        REXLOG_WARN("[sdnx_skin] kept vertices differ from a new skinning: output {:#010x}, {} vertices, first "
                    "difference at byte {} (vertex {})", out, vertices, at, at / 16);
      }
    }
  } else {
    g_misses.fetch_add(1, std::memory_order_relaxed);
    g_miss_vertices.fetch_add(vertices, std::memory_order_relaxed);
  }
  g_cache_bytes -= e.vertices.size();
  e.vertices.assign(out_host, out_host + bytes);
  g_cache_bytes += bytes;
  e.inputs = inputs;
  e.tick = ++g_tick;
  EvictLocked();
}
