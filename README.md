<div align="center">

<img src="docs/logo.png" alt="Sleeping Dogs" width="420">

### Xbox 360 static recompilation for Nintendo Switch

The Xbox 360 release of **Sleeping Dogs**, lifted instruction by instruction to C++ with
[ReXGlue](https://github.com/rexglue/rexglue-sdk) and running natively on the Switch, drawn by its own deko3d renderer.

![Switch](https://img.shields.io/badge/Nintendo%20Switch-homebrew-E60012?logo=nintendoswitch&logoColor=white)
![Xbox 360](https://img.shields.io/badge/Xbox%20360-recompiled-107C10?logo=xbox&logoColor=white)
![deko3d](https://img.shields.io/badge/GPU-deko3d-5C2D91)
![Version](https://img.shields.io/badge/version-0.1.0-blue)
![Status](https://img.shields.io/badge/status-WIP-orange)

[Features](#-features) · [Status](#-status) · [Performance](#-performance) · [Renderer](#-the-native-renderer) · [Building](#%EF%B8%8F-building) · [Roadmap](#%EF%B8%8F-roadmap) · [Legal](#%EF%B8%8F-legal)

</div>

---

> [!WARNING]
> **Work in progress.** The game boots, plays its videos and runs gameplay on a real console without crashing
> (17+ minute sessions), but at ~15–19 fps in handheld mode, below the 30 fps target. There is no prebuilt NRO.

> [!IMPORTANT]
> **No game data is included.** You need your own copy of the Xbox 360 disc. The recompiled code, the shader
> packages and the NRO are made on your computer from it.

##  Features

-  **Native code**: the retail executable (`default.xex`) recompiled to AArch64 with ReXGlue on a Switch fork of
  its SDK (devkitA64, libnx).
-  **Native renderer** (`renderer/`): the game's Xenos command stream drawn directly instead of emulating the Xenos
  GPU, with shaders translated ahead of time. On the Switch it draws through **deko3d**, the low-level Maxwell API;
  the Windows bench runs the same frontend on a Vulkan backend.
-  **Render scale** 100 / 75 / 50 % for the 3D scene, with the UI kept at full resolution and contrast-adaptive
  sharpening (CAS) when the frame is presented.
-  **Videos** (Square Enix / UFG logos, cutscenes), **audio** through the SDK's XMA decoder.
-  **Switch branding patch** (`port/tools/apply_branding.py`): "Xbox 360" texts and button glyphs replaced by
  Nintendo Switch ones in your data files.
-  **Frame limit** 30 / 60 / unlimited (`sdretail.toml`), measured fps in the log.
-  **Profiling on the console**: per-pass GPU timings with shader names, per-thread CPU load and sampled guest
  stacks (`rex_profile.log`), symbolized against the saved ELF.
-  **Tooling**: data formats (`.big`/`.bix` archives, text tables, textures), offline shader library builders
  (SPIR-V and DKSH), bench automation (recorded input replay to gameplay, frame dumps).

##  Status

| Area | State |
| --- | --- |
| Boot, menus, intro videos | ✅ Work on the console |
| Gameplay | ✅ Runs on the console; 17.5 minutes without a crash in the last test, including returns to the main menu and "Continue" |
| Rendering (console, deko3d) | ✅ Correct picture: lighting, shadows, haze, post-processing. Known glitches: lens flare visible through objects, some UI icons partly garbled, glass shimmering while moving |
| Rendering (bench, Vulkan) | ✅ Matches the Xenos emulation |
| Audio | ✅ Works, no delay |
| Performance | ⚠️ ~15–19 fps in handheld gameplay at stock clocks (target: 30), GPU-bound now |
| Online | ⚠️ The game asks to sign in to Xbox LIVE (menu badge, social hub, DLC menu); network prompts still to be removed |
| Branding patch | ⚠️ Text and buttons ready; Xbox LIVE prompts are not covered yet |
| Exit | ⚠️ Closing the game ends in a system module error (pcv / clock service) |

Fixed since the first console builds:
- the crash after a few minutes in a game worker thread (allocator pool corruption): guest memory accesses are
  `volatile` by default, as the game's lock-free code expects;
- a kernel resource exhaustion (`0xCE01`) on "Continue" after returning to the main menu: texture write watches
  work on 64 KB granules and re-map pages in contiguous runs;
- functions reached only through pointers that the recompiler had missed (`nx/sdretail/overrides.toml`);
- on deko3d: black screen, GPU page faults with vertex pulling (stock `uam` loses the upper half of storage buffer
  addresses; a patched, deterministic `uam` is used), striped depth, missing shadows (depth extraction, clip
  distances).

##  Performance

Handheld, stock clocks (no overclocking), 75 % render scale, frame limit 30:

| Scene | Draws per frame | fps (0.1.0, Vulkan/NVK) | fps (now, deko3d) |
| --- | --- | --- | --- |
| Light scenes | < 1500 | — | 18.5 |
| Medium | 1500–2500 | 9 (cutscene) | 17.3 |
| Heavy (night market, streets) | > 2500 | 6.5–7 | 15.6 |

Docked, the same build runs about 25–35 % faster on the GPU side.

What got it there:
- **deko3d instead of Mesa NVK**: much lower driver overhead per draw.
- **GPU**: render scale (75 % gave +60–70 %), early Z through shader variants without alpha test, ZCULL with
  invalidation after depth writes outside draws, depth and stencil extracted in one pass.
- **GPU command thread**: CPU-cached upload buffers flushed by range, draw state recorded only when it changes
  (5–47 thousand commands a frame skipped), register writes in batches, a deeper submit queue (4 in flight),
  best-fit geometry allocator, constant uploads without per-draw comparisons.
- **Recompiled game code**: `volatile` removed from the 964 hottest guest functions that have no spin-wait loops
  (`nx/tools/hot_no_volatile.py`, list in `nx/sdretail/hot_nonvolatile.txt`), +20–30 % fps; a capped spin in
  `RtlEnterCriticalSection`.

Where the time goes now (handheld, average frame): the GPU takes 39–65 ms (~57 ms): scene ~10 ms, lighting ~8,
post-processing ~7, half-resolution effects ~4, final image with UI ~2, and ~14 ms of emulation overhead (depth
extraction, color blits for resolves, full barriers, copies). On the CPU the GPU command thread takes ~50 % of a
core, the game's main thread ~38 %, Wwise ~17 %, four worker threads ~24 % each.

##  The native renderer

`renderer/` is the renderer for ReXGlue that replaces the emulation of the Xenos GPU for this game. It is a snapshot
of the code as developed in the SDK fork (`port/tools/export_renderer.ps1`), meant to be reusable by other Xbox 360
ports on the same SDK. A frontend turns the Xenos command stream into draws; backends behind the `GpuBackend`
interface (`renderer/include/gpu_backend.h`) execute them:

- **Backends**: `deko3d_backend.cpp` for the Switch (on the deko3d core in `nx/deko3d`), `vulkan_backend.cpp` for
  the Windows bench. Specification: [docs/deko3d-backend.md](docs/deko3d-backend.md) (Russian).
- **Shaders ahead of time**: every Xenos shader of the game translated by XenosRecomp (`nx/shaders`) to HLSL, then
  to SPIR-V (DXC) for Vulkan or to DKSH (DXC → SPIRV-Cross → uam, `nx/shaders/tools/dk_build.py`) for deko3d, looked
  up by microcode hash at run time. Vertex fetches with computed indices (manual instancing) read their data in the
  shader (vertex pulling).
- **EDRAM-lite**: render targets as host images keyed by EDRAM base and pitch in samples, resolves as copies into
  host textures (with the copy's R/B swap and exponent bias), depth and stencil restore, depth-as-color clears,
  optional scaled render targets.
- **Geometry**: index and vertex ranges copied once into a persistent buffer and found again by content hash;
  packed normals (10:11:11) repacked.
- **Textures**: tiled and compressed formats (DXT, CTX1 to R8G8), page write watches plus content hash, cube
  faces assembled from resolves.
- **Recording**: only state changes are recorded, occlusion queries answered from host queries, GPU timestamps per
  pass for the profile.

##  Building

Not packaged for others yet; the steps are written down for this machine in [docs/repro.md](docs/repro.md) and
[docs/switch-bringup.md](docs/switch-bringup.md) (Russian). In short:

1. Extract the disc (`port/tools/extract_iso.py`) and the game's shader containers (`port/tools/sdshaders.py`).
2. Build the shader library (`port/tools/shaderlib/build_shaderlib.sh`, WSL: XenosRecomp + DXC), then the deko3d
   package from it (`nx/shaders/tools/dk_build.py`, needs SPIRV-Cross and the patched `uam`).
3. Generate the recompiled code and build the NRO in the devkitA64 Docker image (`nx/tools/sd_codegen.sh`,
   `nx/tools/sdnx_sync_sdk.sh`, `nx/tools/sd_build_nro.sh`) against the Switch SDK fork.
4. Copy `sdretail.nro`, `sdretail.toml` and `sdnx_dksh/` to `sdmc:/switch/sdretail/` together with your game data.

`nx/desktop` builds the same game for Windows with the Vulkan backend: the bench where rendering is debugged.

##  Roadmap

1. **Performance to 30 fps at stock clocks**:
   - GPU: find out which effect each heavy pass is (shader names are in the profile now) and make the costly ones
     cheaper or optional; cut the emulation overhead (depth extraction straight into resolve images, fewer color
     blits and full barriers);
   - CPU: more guest functions without `volatile`, cheaper draw setup on the GPU command thread.
   - Overclocking is not part of the plan; it comes last, if at all.
2. **Rendering glitches**: lens flare through objects, garbled UI icons, glass shimmer.
3. **Offline mode**: hide the Xbox LIVE sign-in prompts and the online badge.
4. **Exit crash** in the system modules.
5. Later: DLC with a Russian translation, the Definitive Edition camera, language selection in the settings, a move
   to a newer ReXGlue release.

##  Legal

Unofficial fan project with no affiliation to Square Enix, United Front Games or Nintendo. "Sleeping Dogs" and its
logo are trademarks of their respective owners. You provide your own legally obtained copy of the game; nothing
derived from it is in this repository, and no prebuilt NRO is published. Running homebrew requires custom firmware,
which violates Nintendo's terms and can get a console banned.

The code of this repository is under the [MIT license](LICENSE). Third-party components and their licenses:
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
