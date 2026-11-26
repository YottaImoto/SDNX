#include <rex/app/title_app.h>
#include <rex/cvar.h>
#include <rex/kernel/title_paths.h>
#include <rex/logging/logger.h>
#include <rex/platform/file.h>
#include <rex/settings_registry.h>
#include "sdretail_init.h"

#include <sys/iosupport.h>
extern "C" const devoptab_t dotab_stdnull;
extern "C" void RexSwitchApmRequestGpuMhz(int mhz);
extern "C" void RexSwitchApmApply();
extern "C" void RexSwitchMarkOrderlyExit();
extern "C" void SdnxMallocThreadCache(bool on);
extern "C" void SdnxMallocCentral(bool on);
void SdnxVerifyData(const std::filesystem::path&);
REXCVAR_DECLARE(bool, sdretail_verify_data);
REXCVAR_DEFINE_BOOL(sdnx_malloc_thread_cache, true, "Sleeping Dogs",
                    "Small host allocations from a per-thread cache, without newlib's process lock (SDNX 16.230)");
REXCVAR_DEFINE_BOOL(sdnx_malloc_central, true, "Sleeping Dogs",
                    "Blocks freed by another thread go back through per-class central lists, not newlib (SDNX 16.245)");
// SDNX 16.246: here and not in the SDK's window_switch.cpp (16.244), which this build does not link.
extern "C" void RexSwitchPerfReadClocks(int enabled);  // SDK src/ui/switch_perf.cpp
REXCVAR_DEFINE_BOOL(switch_read_clocks, true, "Sleeping Dogs",
                    "Read the CPU/GPU/memory clocks (pcv) and temperatures (ptm) once per second for the clocks line of "
                    "rex_profile.log; each read opens and closes its sessions. false: never connect to them");
REXCVAR_DEFINE_INT32(sdretail_handheld_gpu_mhz, 460, "Sleeping Dogs", "Handheld GPU profile (0, 384 or 460 MHz)");
// SDNX 16.258: the game as a player runs it - from the end of startup, no log lines (log.level "off"; categories
// with a level of their own in [log.levels] keep it), no rex_profile.log reports and no stack sampling (the
// overlays keep FPS and resolution), and the title's own measurements off.
extern "C" void RexSwitchPerfQuiet(int quiet);  // SDK src/ui/switch_perf.cpp
REXCVAR_DEFINE_BOOL(sdretail_quiet, false, "Sleeping Dogs",
                    "No logging, profiling or measurements after startup, as for a player (SDNX 16.258)");

class SleepingDogsApp final : public rex::app::TitleApp {
 public:
  SleepingDogsApp() : TitleApp("sdretail", kPPCImageConfig) {}

 protected:
  rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override {
    rex::cvar::RegisterLegacy();
    const auto layer = rex::SettingLayer::kTitle;
    const auto folder = rex::platform::GetExecutableFolder();
    if (auto res = settings.SetText(layer, "app.root", folder.string()); !res) return res;
    if (auto res = settings.SetText(layer, "title.game_root", (folder / "game_root").string()); !res) return res;
    // SDNX 16.222: the saves where the SDNX fork builds keep them (<NRO folder>/sdretail/<xuid>/53510811/...), so
    // both builds share them. The new SDK's default, <NRO folder>/user, would start without the player's saves.
    if (auto res = settings.SetText(layer, "app.user_root", (folder / "sdretail").string()); !res) return res;
    for (const char* key : {"apu.driver", "hid.drivers", "ui.driver"}) {
      if (auto res = settings.SetText(layer, key, "switch"); !res) return res;
    }
    if (auto res = settings.SetText(layer, "input_swap_xy", "true"); !res) return res;
    return settings.SetText(layer, "gpu.driver", "sdnx");
  }

  rex::Result<void> OnInit() override {
    RexSwitchPerfReadClocks(REXCVAR_GET(switch_read_clocks) ? 1 : 0);
    SdnxMallocCentral(REXCVAR_GET(sdnx_malloc_central));
    SdnxMallocThreadCache(REXCVAR_GET(sdnx_malloc_thread_cache));
    const int mhz = REXCVAR_GET(sdretail_handheld_gpu_mhz);
    if (mhz > 0) {
      RexSwitchApmRequestGpuMhz(mhz >= 460 ? 460 : 384);
      RexSwitchApmApply();
    }
    if (REXCVAR_GET(sdretail_verify_data)) SdnxVerifyData(GetTitleRuntime().GetTitlePaths().game_root.Get());
    REXLOG_INFO("[sdnx_next] launching Sleeping Dogs from {}", GetTitleRuntime().GetTitlePaths().game_root.Get().string());
    REXLOG_INFO("[sdnx_next] saves in {}", rex::cvar::GetFlagByName("app.user_root"));
    if (REXCVAR_GET(sdretail_quiet)) {
      RexSwitchPerfQuiet(1);
      rex::cvar::SetFlagByName("sdretail_cull_profile", "false");
      REXLOG_INFO("[sdnx_next] quiet mode (sdretail_quiet): no more log lines, profile reports or measurements");
      (void)REX_LOGGER().level.Set("off");
    }
    return rex::Ok();
  }
};

int main(int argc, char** argv) {
  if (!devoptab_list[STD_OUT]) devoptab_list[STD_OUT] = &dotab_stdnull;
  if (!devoptab_list[STD_ERR]) devoptab_list[STD_ERR] = &dotab_stdnull;
  const int result = rex::app::detail::RunGuarded([&] {
    SleepingDogsApp app;
    if (app.Init(rex::CLIArgList::Parse(argc, argv)) && app.LaunchTitle()) app.Run();
    return app.Shutdown();
  });
  RexSwitchMarkOrderlyExit();
  return result;
}
