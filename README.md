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

[Features](#-features) · [Status](#-status) · [Performance](#-performance) · [Current ReXGlue](#-the-port-to-the-current-rexglue) · [Renderer](#-the-native-renderer) · [Building](#%EF%B8%8F-building) · [Roadmap](#%EF%B8%8F-roadmap) · [Legal](#%EF%B8%8F-legal)

</div>

---

> [!WARNING]
> **Work in progress.** The game boots, plays its videos and runs gameplay on a real console without crashing, now
> on the current ReXGlue SDK (see [The port to the current ReXGlue](#-the-port-to-the-current-rexglue)), but at
> ~19–27 fps in handheld gameplay with sys-clk (~19–20 while driving through the city), below the 30 fps target.
> There is no prebuilt NRO.

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
| Gameplay | ✅ Runs on the console; 28 minutes without a crash in the longest recent test, including returns to the main menu and "Continue". Winston's bus mission stalled after the night-market drop-off (a voice line that never finished, see below); fixed in 16.239 |
| Rendering (console, deko3d) | ✅ Correct picture: lighting, shadows, haze, post-processing. Known glitches: lens flare visible through objects, some UI icons partly garbled, glass shimmering while moving |
| Rendering (bench, Vulkan) | ✅ Matches the Xenos emulation |
| Audio | ✅ Works, no delay; decoded XMA frames cached (95 % of requests answered from the cache, checked bit for bit) |
| Performance | ⚠️ ~20–30 fps in handheld gameplay with sys-clk (target: 30), PGO + LTO build; 26–27 on foot (30 in some places), ~21–22 driving. The GPU takes ~30 ms a frame; while driving the three CPU cores the game may use are full (290–299 % of 300 %) |
| SDK | ✅ Moved to the current ReXGlue (upstream nightly), no Vulkan in the Switch build; saves shared with the older builds |
| Online | ⚠️ The game asks to sign in to Xbox LIVE (menu badge, social hub, DLC menu); network prompts still to be removed |
| Branding patch | ⚠️ Text and buttons ready; Xbox LIVE prompts are not covered yet |
| Exit | ⚠️ The clock service (pcv) has aborted twice, out of session memory, while the overclock tools (Horizon OC / hoc-clk) changed the clocks with the game running: once on exit, once while loading. The game only reads the clocks, and since 16.244 holds no session to pcv between reads (`switch_read_clocks = false` disconnects it completely) |

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

The current-SDK build on the console, handheld, sys-clk CPU 1785 MHz / GPU 921.6 MHz / memory 1600 MHz, 75 % render
scale, frame limit 30, averages over 10-second windows of the profile (driving: through the city, ~4600 draws a
frame; the routes are similar, not identical):

| Build | What changed | On foot | Driving | Worker threads per frame (% of a core) |
| --- | --- | --- | --- | --- |
| 16.248 | VMX registers v0–v13 as locals | — | 20.9 | 6.67 |
| 16.253 | the game's memcpy / memset native, a fresh PGO profile | — | 20.7 | 6.70 |
| 16.254 | pedestrians and traffic × 0.75 | 26.4 | 21.5 | 6.42 |
| 16.255 | the fog volume every 2nd frame | 27.6 | 22.8 | 5.82 |

30 fps in some places on foot. While driving the GPU takes ~31 ms of the 33 a 30-fps frame allows, and the frame
(~43 ms) is held by the CPU (290–299 % of the three cores the game may use). 16.256–16.258 (fog computed once, smaller
shadow casters dropped) are not measured yet.

The fork build (SDK 0.10), stock clocks (no overclocking), the same render scale and frame limit:

| Scene | Draws per frame | fps (0.1.0, Vulkan/NVK) | fps (fork, deko3d) |
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

Where the time goes now (the current-SDK build 16.254, handheld, driving through the city, sys-clk CPU 1785 MHz /
GPU 921.6 MHz / memory 1600 MHz): the GPU takes ~30 ms a frame (the memory clock matters more than the GPU clock:
29–33 ms at GPU 768 MHz with memory at 1600 MHz, 35 ms at GPU 998 MHz with memory at 1331 MHz). The frame (~46 ms) is
held by the CPU: of the three cores the game may use, the game's worker threads take ~138 %, its main thread ~62 %,
the GPU command thread ~58 % (~4600 draws a frame at ~4.6 µs each) and Wwise ~27 %. The hottest game functions are
jobs of the engine, named in its task descriptors: `UpdateFogTask` (the fog volume, computed on the CPU every frame,
~11 % of all game-thread time), `CullManager_CullBucketTask` (visibility, ~10 %) and the CPU skinning of the cars
with their damage (`SkinVehicleBufferTask`, `MorphSkinDamageBufferTask`, `SkinDamageBufferTask`, ~15 %). The
visibility jobs run once per view: measured per view (16.257, `sdretail_cull_profile`) they take ~25 % of a core while
driving, a third for the camera (8–9 %) and two thirds for the three shadow cascades (5 / 5 / 6.5 %).

##  The port to the current ReXGlue

The Switch build first ran on a fork of ReXGlue 0.10. It now runs on the current upstream SDK (its module and driver
interfaces, flat settings), with the Switch parts brought over as drivers: audio (audout), input (HID), UI (NWindow,
applet events) and the renderer as the `sdnx` Xenos graphics driver on deko3d. The title app for it is in
`nx/upstream/` (`main.cpp`, `CMakeLists.txt`, the code-generation options in `perf_overrides.toml`). The SDK side
of this port is not published yet.

What the new build needed to reach and pass the old one (measured on the console, handheld, sys-clk
CPU 1963 MHz / GPU 998 MHz):

| Change | Effect |
| --- | --- |
| Exception entry without the deadlock, hot read-only pages unwatched, host ticks from the system counter | the game reaches gameplay at its real speed (the clock had run 52× fast) |
| `[vector_math] "fused"` in the code generator: VMX multiply-add as one fused instruction, dot products truncated as the console does | cutscenes 10 fps → playable; no Wwise NaNs |
| Per-thread cache in front of newlib's allocator (`nx/sdretail/src/sdnx_malloc.cpp`, `--wrap`) | the GPU command thread no longer waits on the process-wide malloc lock (31 % of its samples); one draw 9–12 → ~5–7 µs |
| Game code at `-O3`, then **LTO** of the game code, the app and the SDK (`SDNX_LTO`, its own build tree via `REXGLUE_OUTPUT_SUFFIX`) | one draw ~5 µs as in the fork; the GPU command thread stops waiting for the game |
| Threads wait on Horizon's address arbiter, XMA kicks without the context lock (renut_nx_ultimate_base ports 65 and 120) | fewer wake-up stalls between game threads and audio |
| Resolve clears only within the resolve rectangle (16.233) | GPU 53 → ~44 ms a frame: the EDRAM-height (2048-row) targets had been cleared whole for a ~720-row frame |
| **PGO** on top of LTO (16.235–16.236): the game code, the app and the SDK modules (now including the SDK's object libraries) profiled in a 28-minute session on the console, then rebuilt with the profile (`SDNX_PGO=generate\|use`) | 17–20 → 19–24 fps; the frame is held by the GPU again |
| XMA loops whose end lies before the first frame are not looped (16.239, a fix the fork had) | voice lines play to their end; the bus mission no longer stalls |
| Cache of decoded XMA frames (16.240, after renut_nx_ultimate_base port 134), keyed by the packet bytes themselves | 95 % of decodes answered from the cache, 0 differences in 8125 checks; small CPU gain, Wwise's own mixing dominates its thread |
| `[vector_math] "native"` for `vrefp` / `vrsqrtefp` (16.241): the ARM reciprocal estimates (FRECPE / FRSQRTE) plus one Newton step, ~16 bits where the Xenon gives 12, instead of a full divide and square root | 1943 call sites; not visible in fps while the GPU holds the frame |
| Depth extraction reads the depth image through a colour view (16.242, `sdnx_depth_alias`) instead of copying its raw texels to a buffer first; enabled only after a self-test compares both ways bit for bit | GPU 46 → ~41 ms (handheld), ~37 ms (docked). Depth written straight to the textures (MRT) is off again: with it the shadows of small objects appeared only 3–5 m ahead of the player |
| Pixel shaders without the texture sign emulation (16.243, `sdnx_zero_signs`): every texture fetch of a translated shader ran a four-channel loop for signed / biased / gamma channels. `nx/deko3d/shaders/build_zero_sign_variants.py` builds a variant of each of the 834 pixel shaders without it (8.7 → 2.7 MB of code, the lighting shader 16 → 3 KB); the renderer binds it only while every sign byte of the draw is 0, so the result is the same bit for bit | GPU ~41 → ~35 ms (handheld): the lighting pass 1.6–1.8 → 1.2 ms, the shadow blur 0.7–0.8 → 0.3 ms; every draw took the variant. Some yellow spots on a shirt disappeared with it |
| Waits for up to 8 kernel objects keep their lists on the stack (16.245, `thread_wait_no_alloc`), and the per-thread allocation cache hands blocks freed by another thread back through per-class central lists (`sdnx_malloc_central`) | newlib's lock: GPU command thread 14 → 0.8 % of its waits, worker threads ~10 → 0 %; the GPU command thread ~4 ms a frame faster. No fps change: the freed time went to the next lock |
| Event waits on Horizon's address arbiter back on (`thread_event_address_wait`; it had been turned off while looking for the bus-mission stall, whose cause was the XMA loop above) | the main thread's time on locks 42 → 20 %, the workers' 38 → 23 %; again no fps change: the three cores stay full |
| `vperm` / `vperm128` on AArch64 as `tbl` + `tbx` (16.246) instead of the SSE form through simde (8 NEON instructions → 5), equal for every control byte (1 024 000 cases on the PC, and a self-test at startup); a single two-register `tbl` was shorter but made the register allocator spill 40–95 % more | the five hottest game functions 8–204 host instructions shorter |
| VMX registers v0–v13 as locals inside each recompiled function (16.247–16.248, `vector_args_as_local` in `perf_overrides.toml`): loaded from the context on entry, written back only around calls and returns, and around each call only the registers the callee (transitively) uses (`nx/tools/vargs_call_sync.py`) | loads and stores of vector registers in the hot functions down by 10–35 %; the worker threads' time per frame −8 % |
| The game's `memcpy` and `memset` (XDK library VMX loops) done by the host's (16.251, `sdretail_native_memcpy`, `nx/sdretail/src/sdnx_perf_hooks.cpp`); the originals still run for overlapping ranges and ranges that cross into the physical view | ~2.5 → ~0.7 % of a core |
| A fresh PGO profile for the current code (16.253) | within noise: the profile had little left to find |
| Fewer ambient pedestrians and traffic cars (16.254, `sdretail_ped_density` / `sdretail_traffic_density`, `nx/sdretail/src/sdnx_world_density.cpp`): the game's own caps (45 pedestrians standing, 35 at speed, 22 cars) and the pedestrian spawn density × the factor, 0.75 by default (34 / 26 pedestrians, 17 cars); scripts that clear the streets still work | driving 20.7 → 21.5 fps, the worker threads' time per frame −4 % |
| The fog volume every 2nd frame (16.255, `sdretail_fog_update_interval`, `nx/sdretail/src/sdnx_fog_interval.cpp`): `UpdateFogTask` fills a 256 KB volume texture on the CPU (32 slices, an `exp` per texel); in the frames in between its jobs return at once and the texture keeps the previous result (skipped only while the texture address stays the same). Since 16.256 `0` computes it only once for each new fog texture (in practice when the world loads), and it no longer follows the time of day or the camera | driving 21.5 → 22.8 fps, the worker threads' time per frame −9 %; `UpdateFogTask` 10.7 → 5.4 % of the game threads at 2, ~0 at 0 |
| CPU car skinning reused when nothing it reads changed (16.256, `sdretail_vehicle_skin_cache`, `nx/sdretail/src/sdnx_vehicle_skin_cache.cpp`): without a damage decal request the job writes only its output vertices, so all of its inputs (bones through the remap, three vertex streams, a per-bone table) are hashed and the vertices kept from the last identical skinning are copied; a sampled byte-for-byte check (`sdretail_vehicle_skin_verify`) | correct (0 differences) but under 1 % reuse: the inputs change every frame even for parked cars. Off |
| Smaller shadow casters dropped (16.258, `sdretail_cull_min_size_shadow`, `_camera`, `nx/sdretail/src/sdnx_cull_profile.cpp`): each view's minimum projected size (an object below it is not drawn) × a factor. The shadow cascades keep objects 2–3× smaller than the camera does (0.03–0.05 against 0.1); 2.0 by default for the shadows | fewer shadow draws (small objects far away cast no shadow); not measured on the console yet |
| Player mode (16.258, `sdretail_quiet`): after startup no log lines, no `rex_profile.log` reports and no stack sampling (the overlays keep fps and resolution), the title's measurements off | for testing the game as a player runs it |

Every one of them has a switch in `sdretail.toml` (the code-generation ones in `perf_overrides.toml`); `vperm` has
none, since checking a switch in every permute would add a branch to every game function that has one. Next: the
shadow cascades (their visibility jobs and draws), the remaining skinning jobs, a few unnamed hot game functions, the
GPU command thread's per-draw cost.

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
  shader (vertex pulling). `nx/deko3d/shaders/build_zero_sign_variants.py` adds, next to the package, variants of
  the pixel shaders without the texture sign emulation (`sdnx_dksh/zs/`, with an index of the installed shaders'
  sizes and hashes, so a variant is only used with the exact shader it was made from).
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

The current-SDK build: `nx/upstream/` is added to the Switch SDK's CMake tree as its title directory
(`REXGLUE_SWITCH_TITLE_DIR`); `-DSDNX_LTO=ON -DREXGLUE_OUTPUT_SUFFIX=-lto` gives the LTO build in a tree of its own.
For PGO: build with `-DSDNX_PGO=generate` (its own tree), play on the console (the counts are written every 2 minutes
to `pgo/sdnx_pgo_<N>.stream` next to the NRO), turn the stream into `.gcda` files with
`SDNX_PGO_HOME=$HOME/sdnx-next nx/tools/sd_pgo_merge.sh <folder>`, then build with `-DSDNX_PGO=use -DSDNX_LTO=ON`.
The profile belongs to that code: regenerate the game code and it has to be taken again.

##  Roadmap

1. **Performance to 30 fps** (measured with sys-clk; stock clocks are the floor):
   - CPU, the limit while driving: the engine's jobs (shadow-cascade visibility, skinning), Wwise's mixing, the GPU
     command thread's per-draw cost; more guest functions without `volatile`;
   - GPU (30–35 ms): the remaining emulation overhead (resolve blits, stencil restores, full barriers) without
     changing the picture; depth written straight to the textures (MRT) without losing the small objects' shadows.
2. **Rendering glitches**: lens flare through objects, garbled UI icons, glass shimmer.
3. **Offline mode**: hide the Xbox LIVE sign-in prompts and the online badge.
4. **The pcv abort** while the overclock tools change the clocks (most likely not the game's: check with
   `switch_read_clocks = false`).
5. **Publish the SDK side** of the port to the current ReXGlue.
6. Later: DLC with a Russian translation, the Definitive Edition camera, language selection in the settings.

##  Legal

Unofficial fan project with no affiliation to Square Enix, United Front Games or Nintendo. "Sleeping Dogs" and its
logo are trademarks of their respective owners. You provide your own legally obtained copy of the game; nothing
derived from it is in this repository, and no prebuilt NRO is published. Running homebrew requires custom firmware,
which violates Nintendo's terms and can get a console banned.

The code of this repository is under the [MIT license](LICENSE). Third-party components and their licenses:
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
