// Sleeping Dogs for Nintendo Switch: fewer ambient pedestrians and traffic cars than the Xbox 360 game spawns (SDNX
// 16.254), to give the CPU back the game threads' work on them (AI, animation, physics, drawing). Off at 1.0.
//
// Pedestrians: sub_822B3798 (every update) recomputes the ambient pedestrian cap at 0x82F328D8 (45 standing, down to
// 35 at 28+ m/s; -1 from a script means 42) and the ambient density at 0x82F32BBC (the hour's PedDensityScale from the
// config table at 0x82FBBE50 x 0.032 x Rain_Adjust, lowered up to 75 % with the player's speed). New pedestrians come
// while cap - (both counts) > 4 (sub_82293DE8, sub_821D6870); sub_827A21C0 answers spawn queries with density x a
// factor per kind. sub_82645D10 is the scripts' cap setter. Both values are scaled by sdretail_ped_density after the
// game writes them; before the next update the unscaled ones go back in, because one branch of the update starts
// from the previous density (scaling it again each update would run it down to zero).
//
// Traffic: the cap of ambient cars at 0x8300E01C is trunc(22 (0x82F3BFDC) x the scripts' set_traffic_density_scale
// (0x82F3BFD4)), written only by sub_8283BFB8 (the script function), sub_8283C060 (new base count, at most 22),
// sub_82EA6368 and sub_8283C0B8 (reset to 22 x 1.0). After each of them the cap is recomputed from those two values
// and scaled by sdretail_traffic_density; spawning compares the car count with it (sub_82853608, sub_82855A38,
// sub_82669DD8).
//
// Scripts that clear or lock pedestrians and traffic keep working: a cap of 0 stays 0. All of it runs on the game's
// main update thread.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/hook.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

REXCVAR_DEFINE_DOUBLE(sdretail_ped_density, 1.0, "Sleeping Dogs",
                      "Ambient pedestrian cap and density x this (SDNX 16.254; 1.0 = the game's, 0.25-1.5)");
REXCVAR_DEFINE_DOUBLE(sdretail_traffic_density, 1.0, "Sleeping Dogs",
                      "Ambient traffic car cap x this (SDNX 16.254; 1.0 = the game's, 0.25-1.5)");

REX_EXTERN(__imp__sub_822B3798);
REX_EXTERN(__imp__sub_82645D10);
REX_EXTERN(__imp__sub_8283BFB8);
REX_EXTERN(__imp__sub_8283C060);
REX_EXTERN(__imp__sub_82EA6368);
REX_EXTERN(__imp__sub_8283C0B8);

namespace {

constexpr uint32_t kPedCap = 0x82F328D8u;          // int32, ambient pedestrian cap
constexpr uint32_t kPedDensity = 0x82F32BBCu;      // float, ambient pedestrian density
constexpr uint32_t kTrafficBase = 0x82F3BFDCu;     // int32, base car count (22)
constexpr uint32_t kTrafficScale = 0x82F3BFD4u;    // float, set_traffic_density_scale
constexpr uint32_t kTrafficCap = 0x8300E01Cu;      // int32, ambient car cap

uint32_t LoadU32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

void StoreU32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, 4);
}

float LoadF32(const uint8_t* base, uint32_t address) {
  const uint32_t bits = LoadU32(base, address);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

uint32_t FloatBits(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  return bits;
}

double Factor(double v) { return std::isfinite(v) ? std::clamp(v, 0.25, 1.5) : 1.0; }

double PedFactor() {
  static const double f = Factor(REXCVAR_GET(sdretail_ped_density));
  return f;
}

double TrafficFactor() {
  static const double f = Factor(REXCVAR_GET(sdretail_traffic_density));
  return f;
}

// A cap of n x f, at least 1 while n is at least 1 (0 and the -1 of the scripts stay as they are).
int32_t ScaleCount(int32_t n, double f) {
  if (n <= 0) return n;
  return std::max<int32_t>(1, int32_t(std::lround(double(n) * f)));
}

// Pedestrian values as the game wrote them and as scaled here (to tell ours from a new write of the game).
struct PedShadow {
  bool valid = false;
  uint32_t cap_game = 0, cap_ours = 0;
  uint32_t density_game = 0, density_ours = 0;
} g_ped;

int32_t g_logged_ped_cap = INT32_MIN, g_logged_traffic_cap = INT32_MIN;
uint32_t g_ped_updates = 0;

// Before the game reads its own values: the unscaled ones back where they are still ours.
void PedRestore(uint8_t* base) {
  if (!g_ped.valid) return;
  if (LoadU32(base, kPedCap) == g_ped.cap_ours) StoreU32(base, kPedCap, g_ped.cap_game);
  if (LoadU32(base, kPedDensity) == g_ped.density_ours) StoreU32(base, kPedDensity, g_ped.density_game);
}

void PedScale(uint8_t* base) {
  const double f = PedFactor();
  const uint32_t cap = LoadU32(base, kPedCap);
  const uint32_t density = LoadU32(base, kPedDensity);
  const int32_t cap_scaled = ScaleCount(int32_t(cap), f);
  float d;
  std::memcpy(&d, &density, 4);
  const uint32_t density_scaled = std::isfinite(d) ? FloatBits(float(double(d) * f)) : density;
  StoreU32(base, kPedCap, uint32_t(cap_scaled));
  StoreU32(base, kPedDensity, density_scaled);
  g_ped = {true, cap, uint32_t(cap_scaled), density, density_scaled};
  // the first time, then one line per ~4096 updates (the cap follows the player's speed every update)
  if (g_logged_ped_cap == INT32_MIN || (++g_ped_updates & 4095) == 0) {
    g_logged_ped_cap = int32_t(cap);
    REXLOG_INFO("[sdnx_density] pedestrian cap {} -> {}, density {:.4f} -> {:.4f} (x{:.2f})", int32_t(cap),
                cap_scaled, d, double(d) * f, f);
  }
}

void TrafficRescale(uint8_t* base) {
  const double f = TrafficFactor();
  const int32_t n = int32_t(LoadU32(base, kTrafficBase));
  const float scale = LoadF32(base, kTrafficScale);
  // as the game: fcfid, frsp, fmuls, fctidz
  const float product = float(double(n)) * scale;
  const int32_t cap = std::isfinite(product) ? int32_t(product) : int32_t(LoadU32(base, kTrafficCap));
  const int32_t scaled = ScaleCount(cap, f);
  StoreU32(base, kTrafficCap, uint32_t(scaled));
  if (cap != g_logged_traffic_cap) {
    g_logged_traffic_cap = cap;
    REXLOG_INFO("[sdnx_density] traffic cap {} -> {} (base {} x scale {:.2f}, x{:.2f})", cap, scaled, n, scale, f);
  }
}

}  // namespace

REX_HOOK_RAW(sub_822B3798) {
  if (PedFactor() == 1.0) {
    __imp__sub_822B3798(ctx, base);
    return;
  }
  PedRestore(base);
  __imp__sub_822B3798(ctx, base);
  PedScale(base);
}

REX_HOOK_RAW(sub_82645D10) {
  if (PedFactor() == 1.0) {
    __imp__sub_82645D10(ctx, base);
    return;
  }
  PedRestore(base);
  __imp__sub_82645D10(ctx, base);
  PedScale(base);
}

#define SDNX_TRAFFIC_HOOK(name)                     \
  REX_HOOK_RAW(name) {                              \
    __imp__##name(ctx, base);                       \
    if (TrafficFactor() != 1.0) TrafficRescale(base); \
  }

SDNX_TRAFFIC_HOOK(sub_8283BFB8)
SDNX_TRAFFIC_HOOK(sub_8283C060)
SDNX_TRAFFIC_HOOK(sub_82EA6368)
SDNX_TRAFFIC_HOOK(sub_8283C0B8)
