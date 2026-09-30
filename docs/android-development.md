# Android development

Current status (2026-09-24): **experimental; Halo Reach plays its campaign
with correct lighting and colours on the tested Odin2 Portal**. Menus, input,
the intro cinematic and first-person gameplay with HUD render like the PC
reference at about 15 fps in gameplay (optimized native build); see
[Performance](#performance-2026-09-25). A successful build or a visible menu does not establish game
compatibility.

See [Verified state](#verified-state-2026-09-24) for what was measured, and
[Trace diff](#trace-diff-finding-rendering-bugs) for how rendering bugs are
located.

This guide is the entry point for Android work in this checkout. Historical
work orders and AI reports describe past experiments; their conclusions need
current source and device evidence before reuse.

## Source and device

The active Windows checkout is `E:\xbox360emu-build\XeniOS-source`. The
`C:\Users\conor\OneDrive\Personal\Documents\xbox360emu` folder holds device
artifacts and backups. Its `cleanroom-x360` subdirectory is a separate project.

Test device: Odin2 Portal, serial `adf63ecd`, ARM64, 4 KB pages, about 7 GiB
usable physical RAM. Debug package: `jp.xenios.emulator.github.debug`.
The current test game is Halo Reach. Other games are unverified by this audit.

## Build and checks

On the current Windows development machine, from the source root:

```powershell
$env:JAVA_HOME = 'C:\Program Files\Microsoft\jdk-17.0.18.8-hotspot'
Set-Location android\android_studio_project
.\gradlew.bat --offline --no-daemon "-Pandroid.aapt2FromMavenOverride=$env:LOCALAPPDATA\Android\Sdk\build-tools\33.0.2\aapt2.exe" assembleGithubDebug
```

This uses the locally installed SDK/NDK and cached Gradle dependencies. JDK 17
is required by this tested setup; the old D8 compiler failed under JDK 21.
The AAPT2 override avoids an uncached Maven artifact on this machine. A fresh
machine needs its own SDK/NDK paths and dependency setup.

The APK is `android/android_studio_project/app/build/outputs/apk/github/debug/app-github-debug.apk`.
Update with `adb install -r <apk>` to preserve app data. Do not uninstall or
clear storage as part of a routine update.

That build's native code is unoptimized (`-O0`). For performance testing, build
the same debuggable `.debug` package (same data, saves and `run-as` access)
with optimized native code, arm64 only:

```powershell
.\gradlew.bat --offline --no-daemon "-Pandroid.aapt2FromMavenOverride=$env:LOCALAPPDATA\Android\Sdk\build-tools\33.0.2\aapt2.exe" "-PxeniaNativeConfig=Release" "-Pandroid.injected.build.abi=arm64-v8a" assembleGithubDebug
```

The single-ABI build is marked test-only and written to
`app/build/intermediates/apk/github/debug/app-github-debug.apk`; install it
with `adb install -r -t <apk>`. The Release sections of `build/*.prj.Android.mk`
are hand-maintained. If a source file is added only to the Debug section, the
optimized link fails with undefined symbols; copy the `LOCAL_SRC_FILES` list.

Run the targeted cleanup regression checks from the source root:

```powershell
.\tools\android\Test-RendererCleanup.ps1 -Compiler C:\Strawberry\c\bin\g++.exe
```

These compile actual source fragments with small host/GPU substitutes. They
check native versus explicit RGBA8 dump selection, cache-key determinism,
restore failure propagation, staging-buffer lifetime on timeout and FSI
capability fallback. They do not execute real Vulkan commands or validate
Xbox GPU accuracy. Android compilation and device tests are also required.

## Configuration

Use UTF-8 text. The earlier internal and external profiles contained UTF-16
NUL bytes and were ignored by the byte-oriented parser.

`files/halo_experiment.txt` controls title-specific experiments separately
from the cvar profile. Some fields reload; others require a restart. Check
the logged applied/ignored fields before interpreting a comparison.

- `repack_mode=0` preserves the source dump format, including 10-bit formats.
  Sample collapse is a separate experiment. Explicit direct-MSAA presentation
  can still request RGBA8 conversion.
- `repack_mode=1` requests RGBA8 conversion for eligible collapsed color
  dumps. Optional color curves change guest color values; they are display
  experiments, not a claim of accurate format emulation.
- `force_fsi` is a legacy alias for requesting FSI. All required hardware
  capabilities must be present. The old capability bypass and draw-barrier
  substitute were removed: interlock orders overlapping fragment invocations,
  including those within a draw. See the [Khronos extension specification](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_fragment_shader_interlock.html).
- `signed_fixed16_resolve_pack` is an active bias experiment, despite its old
  comment claiming otherwise. It subtracts one from selected resolve biases;
  it does not select a signed shader. The related forced-minus-five bias is
  also experimental. Both default off. Current `XePack64bpp4Pixels` source
  uses UNORM packing for formats 21/26; do not assume an EDRAM packing change
  exists because a comment or old report says so.
- `present_cpu_swap_texture=0` tries the tracked host render target directly;
  it can fall back to the normal texture path. Check which source was actually
  used. A comparison reduced scrambled bands but retained incorrect colors
  and surfaces, so this is not a validated scene fix.

Retained experiments are unresolved work. Do not add another default-on
workaround without a reproducible defect, a defined expected result and a
device comparison. Remove a workaround only when its replacement is verified
or its lack of effect is established from all callers.

## Saves and repeatable device checks

Tools → Capture + Save State writes one diagnostic slot at
`<external-files>/diagnostic_save/current.xes`. Tools → Restore Saved State
loads it. Keep a separate copy before replacing a useful diagnostic scene.

Debug builds also accept save/restore from adb, so a test scene need not be
replayed from boot (about 4 minutes of loading and cutscenes). Write one
line to `<external-files>/android_state_cmd.txt`: `save <path>` or
`restore <path>`. The app polls every second, deletes the command file and
writes the native result to `android_state_result.txt` (`ok\t...` on
success). It uses the same native path as the Tools buttons
(`EmulatorActivity.pollStateCommand`). Use your own file names; don't overwrite
`current.xes` or the `*-20260905.xes` backups.

```powershell
$f = '/sdcard/Android/data/jp.xenios.emulator.github.debug/files'
adb shell "am start -W -n jp.xenios.emulator.github.debug/jp.xenios.emulator.EmulatorActivity --es target $f/games/halo\ reach.iso"
# wait ~25 s for the title to boot, then:
adb shell "echo 'restore $f/diagnostic_save/pink_grass.xes' > $f/android_state_cmd.txt"
```

Measured 2026-09-24: save 0.9-2.2 s, restore 0.8 s, and in the mission about
30 s after launch. A save taken during the intro cinematic resumes at the
cinematic's start, not at the saved shot. Saved scenes: `pink_cutscene.xes`
(intro valley shot), `pink_grass.xes` (Noble Team leaving the hangar) and
`gameplay_hud.xes` (first-person gameplay with HUD). Don't run trace replays while the app is running: the
combined GPU memory got the app killed.

The September 5 baseline restored both a menu and a 3D scene after capture;
the 3D scene also restored after restarting the app and continued into the
vehicle tutorial. The measured saves took about 3 seconds, and restoration
about 4 seconds, followed by shader warmup. These timings are observations
for that build, not a performance guarantee.

Validation checks version, title and payload integrity. It does not establish
portability across builds, configurations, game revisions or devices. XMA
decoder state and host render-target rehydration still need deeper validation.
The checked Vulkan restore path now reports allocation, mapping, submission,
timeout and device-loss failures to the caller. A failure after destructive
restore begins leaves emulation paused; restart the app to recover.

For each runtime change: record the APK hash and active configuration, load
the same scene, confirm new game frames and controller response, inspect the
3D image, and record frame rate/memory/errors. A restore-success message or a
moving Android overlay alone is insufficient.

Ordinary Halo checkpoints are separate. See
[checkpoint snapshots](android-halo-checkpoint-snapshots.md) and
[diagnostic capture](android-debug-diagnostic-capture.md).

## Verified state (2026-09-24)

Each item was measured on the device or checked against the PC reference
(see [Trace diff](#trace-diff-finding-rendering-bugs)). See the git log for the commits.

Fixed:

- **Mission 0.1 fps stalls and full-screen noise.** Pixel shaders with guest
  jumps/loops were translated as one program-counter loop plus switch around
  the whole shader. Adreno then spilled registers and allocated 24-72 MB of
  driver (KGSL) memory per pipeline. `spirv_structurize_forward_jumps`
  (default on) translates forward jumps as guarded segments and guest loops as
  real SPIR-V loops. On the heavy mission frame, driver pipeline memory went
  from 3.7 GB to 1.1 GB with an identical final image. KGSL-only memory
  pressure no longer clears render targets
  (`vulkan_kgsl_reclaim_clears_render_targets`, default off). That clear was
  the source of the RGB-noise corruption.
- **Stale CPU-written vertex data.** Vulkan vertex-buffer residency skipped
  `RequestRange` when address and size were unchanged; it now requests every
  draw.
- **Occlusion-query draws.** `VulkanCommandProcessor::SupportsGuestOcclusionQueries`
  ignored `occlusion_query_enable` (default off). Halo's viz-query draws
  (`kill_pix_post_hi_z`) were then issued as ordinary draws that wrote depth
  and color the real GPU discards (11 extra draws per frame). It now matches
  D3D12 and Canary.
- **Frame tracing in optimized builds.** The trace writer is compiled into
  Android NDEBUG builds (`trace_writer.h`).
- **Dark / wrongly lit frames (the "magenta" scenes).** On Adreno, fragment
  shaders that declare the SPIR-V float-controls execution modes
  `DenormFlushToZero` or `SignedZeroInfNanPreserve` read `gl_FragCoord.xy` as
  0. Every shader using the guest's screen-position parameter (PsParamGen,
  75 shaders in the `pink_grass` frame, including depth linearization and
  deferred lighting) then sampled texel (0,0). `SpirvShaderTranslator` no
  longer declares these modes on Qualcomm
  (`spirv_adreno_float_controls_workaround`, default on;
  `spirv_disable_float_controls` disables them on any vendor). Both recorded
  traces now match the PC reference (final frame channel means within 0.2).
  The earlier per-draw RT0 dump observations for draw 1196 predate this and
  should not be reused.
- **Green/magenta colour cast.** The present swizzle override `0xA42`
  compensated for the broken lighting and now tints the frame.
  `swap_swizzle_override` now defaults to 0 (the guest's swizzle). An old
  `halo_experiment.txt` with `swap_swizzle_override=0xA42` still applies it;
  remove that line. `writer_gb_fix` made no visible difference and is left on.
- **Yellow glowing foliage** was `halo_android_compat_presentable_color_shadow`,
  now off in the code default and the bundled profile. Existing installs keep
  their storage copy of the profile; set the key to false there.

Notes:

- **Replays must use the app's configuration.** The trace dump doesn't apply
  the cvar overrides the app forces on Android (`xenia_main.cc`: dynamic
  rendering off, `tiled_shared_memory`, memory limits). Use
  `replay-android.ps1 -AppConfig`. With `vulkan_dynamic_rendering=true`,
  which only the trace dump used, MRT draws wrote their second output into
  RT0.
- **Shader debugging:** `--spirv_debug_ps_hash` with `--spirv_debug_ps_output`
  replaces a pixel shader's oC0 (N >= 0: register rN; -1-N: float constant N;
  -100: literal (1,2,3,4); -800: `gl_FragCoord`). `--trace_dump_texture_slot`
  and `--trace_dump_color0_host` dump host images per draw.

## Performance (2026-09-25)

Measured in `gameplay_hud.xes` (Winter Contingency, first person) with
`trace-harness\perf_run.ps1`. The GPU is the limit: KGSL reports 99% busy at
the maximum 680 MHz, and no CPU thread exceeds about 50%.

| State | fps | GPU ms/frame | Render passes/frame |
| --- | --- | --- | --- |
| Before (2026-09-24 build) | 11.3 | 88 | 448 |
| Upload hoisting | 13.3 | 71 | 256 |
| + hazard-based memexport barriers | 14.9 | 64 | 128 |

Where the time went (GPU timestamps, `vulkan_gpu_timing`): 285 single-draw
render passes cost 35 ms; each Adreno render pass costs about 120 µs whatever
it draws. The passes were split by CPU-to-GPU shared memory uploads (about
190 per frame, vertex and index data written by the CPU each frame) and by
shared memory barriers around memexport draws (about 120 per frame). A
smaller render area did not help (`vulkan_shrink_render_area`, off), so the
cost is per pass, not load/store bandwidth.

Fixed (defaults on for Android):

- **Upload hoisting** (`vulkan_hoist_shared_memory_uploads`). An upload a
  draw needs while a render pass is open is recorded before that pass
  (`DeferredCommandBuffer::HoistBufferCopyBeforeRenderPass`), batched into
  one copy between one pair of barriers, unless a draw already in the pass
  read the range.
- **Hazard-based memexport barriers**
  (`vulkan_shared_memory_hazard_barriers`,
  `vulkan_shared_memory_skip_waw_barriers`). Between draws, a shared memory
  barrier is inserted only for a real read-after-write or write-after-read
  on the exact memexport stream ranges. Reach's 120 per-frame hazards were
  all draws exporting to the same 560 KB stream (Xenia sees the stream as its
  whole buffer), with nothing reading it in between.
- **Persistent pipeline cache.** The shader and pipeline storage files were
  opened with `"a+b"`, which on Bionic starts reading at the end of the file;
  every launch saw an invalid header and wiped them. Now rewound before
  reading (`shader_storage.h`). The driver `VkPipelineCache` is also saved
  every 20 s while pipelines are being created
  (`vulkan_pipeline_cache_save_interval_s`), since Android rarely shuts the
  app down cleanly. Restoring a gameplay save no longer drops to 0.5 fps for
  40 s while 390 pipelines compile.
- **Screen timeout.** The emulator activity keeps the screen on; a timeout
  paused the app and dropped the GPU caches (`TRIM_MEMORY_UI_HIDDEN`).

Remaining per frame (64 ms): 7 large passes 30 ms (real shading), 67
single-draw passes 9 ms (render target transfers and resolve dumps, about 35
each), dispatches 7 ms (116 resolves and texture loads), copies and barrier
waits 6 ms. Translated shaders are large: fragment shaders average 3,800
Adreno instructions, and Reach's per-vertex lighting shader
(VS `838B50F94967ACAD`, used by the memexport draws) 46,000.

Measured and rejected:

- `spirv_guest_zero_multiply=false` (skip the Shader Model 3 "0 × anything =
  0" emulation): 16.1 fps, but HUD text loses glyphs. Keep on.
- `spirv_no_contraction=false`, `spirv_shared_memory_nonuniform_indexing`
  (index the four 128 MB shared memory bindings directly instead of a switch
  per fetch): no change.

Diagnostics (profile keys, all default off): `vulkan_gpu_timing` (GpuTime
line: pass time by draw count, dispatch and copy time),
`vulkan_log_render_pass_breaks` (RPBreak and RPBarrier stacks; symbolize
with `llvm-addr2line` on
`app/build/intermediates/ndkBuild/githubDebug/obj/local/arm64-v8a/libxenia-app.so`),
and `vulkan_log_pipeline_statistics` (Adreno instruction counts per
pipeline). Every summary also logs a GpuWork line (passes, draws, barriers,
copies, hoisted uploads per frame).

Later measurements (2026-09-26, same scene, about 15 fps):

- The emulator shows an FPS overlay (guest frames presented per second) in
  the top-left corner.
- Presenter fix: the swapchain paint pipeline's format was never recorded, so
  every frame waited for the previous present and recompiled the pipeline.
  The command processor thread dropped from 66% to 53% of a core.
- The main 7e3 scene pass (about 16 ms) is roughly vertex work 4 ms, pixel
  shading 6 ms and raster/depth/blending 6 ms (`vulkan_debug_tiny_scissor`,
  `spirv_debug_ps_hash=all` with `spirv_debug_ps_output=-100`,
  `spirv_debug_null_vs`). No single fix remains there.
- A CPU-side cap sits near 20 fps even with the GPU work removed. Resolve
  readback copies 38 resolves (about 46 MB) per frame into guest memory.
  `readback_resolve=none` lifts the cap to about 25 fps but darkens the
  image (the game reads exposure on the CPU). `readback_resolve_max_kb`
  keeps exposure but leaves stale-page artifacts, so readback needs to be
  done on demand.
- No gain: texel-buffer vertex fetch (`vulkan_shared_memory_texel_buffer`),
  non-sparse shared memory, NaN-guarded dot products
  (`spirv_guest_zero_multiply_fast`), and disabling the Halo compatibility
  workarounds (which also didn't change the image).

Profile gotchas:

- The Android profile accepts any registered cvar by name, so experiments
  need no rebuild.
- `files/xenios_android_profile.txt` on external storage loads after the
  internal one and overrides the keys it contains.
- `xenios.config.toml` stores every cvar's value when it's written, so a
  changed code default doesn't apply to an install that already saved the
  old value. Delete the line to get the new default.

### Device recheck (2026-09-30)

On the same Odin2 Portal and `gameplay_hud.xes`, after shader warmup:

| APK / profile | Measured time | Refreshed-output fps |
| --- | --- | --- |
| Installed September 26 APK, existing profiles | 115.84 s | 16.45 |
| Rebuilt optimized APK, existing profiles | 120.89 s | 16.38 |
| Rebuilt optimized APK, bundled profile in both locations | 120.98 s | 16.45 |

No run had pending shader compilation. The installed APK SHA-256 was
`F89CE4D75CFAF947EF8C177E624AE96A0C17A551B3807E84F195F3DD1C8B7395`;
the rebuilt APK was
`BA1AD000923328B50D627D5E3738AFEFF582F65EBDD94F2EDD8942A54FD3CB36`.
Their packaged ARM64 native library bytes are identical. The existing device
profiles already set fast readback and the tested memory budgets, so this
packaging correction does not claim a gameplay speedup on that install.
The bundled-profile test selected a 3595 MiB device-sized KGSL limit.
The updated APK restored the scene and accepted START to open the pause menu.

With GPU timestamps and render-pass break logging enabled, a separate
120.73-second run gave 16.30 fps and 60.24 ms GPU time per swap. The baseline
GPU samples reported 99% busy at 680 MHz. Symbolized pass-break stacks point
to `PerformTransfersAndResolveClears` (about 35 breaks/frame) and
`ExecutePendingDumpRectanglesToEdram` (about 33), followed by texture loading.
These are measured targets for further investigation; data redundancy must
be proven before removing an ownership transfer or dump.

Two colour-conversion probes, using the same scene with GPU timing enabled,
did not produce a useful speedup:

| Conversion | Measured time | Refreshed-output fps | GPU ms/swap |
| --- | --- | --- | --- |
| Committed implementation, control | 120.73 s | 16.30 | 60.24 |
| Existing `arithmetic_7e3_conversion=1` experiment | 115.90 s | 16.27 | 59.97 |
| Exact unpack without a bit scan or variable shift | 115.62 s | 16.29 | 59.92 |

The arithmetic pack probe disagreed with the existing integer conversion
near rounding boundaries; it remains disabled. The narrower unpack candidate
passed 141,312 host checks across all 1024 encodings, all legal source bit
offsets, unrelated surrounding bits and float/uint return modes. Its Android
build restored and rendered the scene, but the measured difference was too
small to justify retaining a renderer change for this performance task. The
candidate was discarded, and the committed optimized APK was reinstalled.
Neither probe had pending shader compilation. Probe sources and results are
retained with the local evidence bundle.

Evidence bundles and pre-test APK/profile/ordinary-save/diagnostic-slot backups
are in the artifacts workspace's `diagnostics-perf-20260930` directory.
After testing, both profiles were restored byte-for-byte. All 32 checked
private save, configuration and launcher-preference files match the original
backup, including recovery of two save/profile files Halo updated during the
runs. The `current.xes` and `gameplay_hud.xes` slot hashes match the pre-test
backups. The committed optimized APK remains installed.

### Transfer batching investigation (2026-09-30)

The target is 30 fps, requiring at most 33.33 ms per frame. The tested native
resolution gameplay scene remains around 16.5 fps. Combining compatible
ownership transfers with the following guest draw pass removes about 18
render passes per frame, but the measured improvement is only about 1%.

`vulkan_transfer_in_draw_pass` exposes this path as an **opt-in experiment**
(default false). It accepts color targets whose native draw format and transfer
format/image view agree, including Reach's 7e3 targets. Depth, integer transfer
views, active attachment sources and dependencies between transfers retain
the standalone path. The legacy `menu_transfer_in_draw_pass` experiment can
still independently queue RGBA8 transfers. Both must be off for a control.
Queued transfers now finish before resolve, EDRAM save capture and submission
completion, including after a skipped draw. Shutdown clears their references;
cache reclamation requires the queue to have been drained.

Same device and `gameplay_hud.xes`, with zero pending shader compilation:

| Batching / diagnostics | Measured time | Refreshed-output fps | GPU ms/swap | Passes/frame |
| --- | --- | --- | --- | --- |
| Off, GPU timing and pass-break logging | 120.65 s | 16.31 | 59.57 | 129.1 |
| On, GPU timing and pass-break logging | 120.80 s | 16.48 | 58.92 | 110.8 |
| On, diagnostics off | 120.90 s | 16.62 | not recorded | 110.9 |

An earlier on/off pair measured 16.47/16.30 fps and 59.03/59.96 ms GPU time.
The gain is small across both comparisons. The earlier unprofiled baseline
was 16.45 fps; it was a separate run, rather than a paired control for the
16.62 fps experiment. Lighting, world geometry and HUD were inspected on the
device. These observations do not establish accuracy in other scenes/games.

The same freshly captured 38-resolve gameplay trace was replayed with a
matching optimized headless binary and the app's configuration. One early
batched/control-repeat pair matched all resolve bytes. Further final-build
replays did not consistently match: repeating the **disabled** control also
changed tiny pixel patches. In the final RGBA8 resolve, one enabled run
differed from its control in 48 of 829,440 pixels (maximum channel delta 12);
a repeated disabled control differed in 42 pixels (maximum delta 4).
These variations are not yet fully explained or attributed, so batching is
kept disabled by default. A visually plausible frame and a small speedup do
not resolve that accuracy question.

Current code and logs also confirm that deferred CPU readback is already on
(`readback_resolve_deferred=true`). It avoids copying most large resolves into
guest CPU memory, but GPU-to-staging copies still run for roughly 38 resolves,
47 MiB per frame. The seven large draw passes still cost about 27 ms and GPU
dispatch/copy work about 12.5 ms. These remain substantially larger targets
than this batching gain. Safely reducing staging work must preserve delayed
readback data, exposure reads and staging-buffer lifetimes; disabling readback
or limiting it by size alone previously broke the image.

Run the scheduling and skipped-draw boundary regressions with:

```powershell
.\tools\android\Test-DrawPassTransfers.ps1 -Compiler C:\Strawberry\c\bin\g++.exe
```

These execute actual source functions/prefixes with small GPU substitutes;
they do not execute Vulkan or prove image equivalence. The existing renderer
cleanup checks and optimized Android build also passed.

Evidence is retained in the artifacts workspace's
`diagnostics-perf-30fps-20260930` directory, including APK/config hashes, paired
measurements, trace replays and pre-test save backups. The final installed
APK SHA-256 is
`D88803B32D6CED03EB5809A2714707FADDF475A5866EA5756B83DE539629BFC9`;
it keeps batching disabled. After testing, all 32 protected private save,
configuration and preference files, both external configuration files and the
`current.xes`/`gameplay_hud.xes` hashes were restored or verified against the
fresh pre-test backups. The final build restored the gameplay scene, logged
129 passes per frame with batching disabled, and accepted START to open and
close the pause menu.

### SPIR-V optimization investigation (2026-09-30)

Enabling the existing `vulkan_spirv_optimization` option did not produce a
useful gameplay speedup. Current code optimizes translated shaders before
creating their Vulkan modules, including synchronous creation; the cvar's
async-only help text is stale. Restart the process when changing this option.

The same Odin2 Portal and `gameplay_hud.xes` were measured with GPU timing
enabled, pass-break logging disabled and the guest arithmetic flags unchanged.
All measured intervals had zero pending shader compilation. The GPU samples
reported 99% busy at 680 MHz.

| APK / optimizer | Measured time | Refreshed-output fps | Reported GPU ms/swap |
| --- | --- | --- | --- |
| Committed APK, off | 120.50 s | 16.33 | 57.33 |
| Committed APK, on | 115.81 s | 16.16 | 57.47 |
| Committed APK, on, longer startup warmup | 115.83 s | 16.24 | 57.45 |
| Temporary filter APK, off | 120.55 s | 16.27 | 57.51 |
| Temporary filter APK, 12 pixel shaders only | 120.54 s | 16.34 | 57.43 |

The first enabled launch's restore timed out during startup compilation. Its
successful retry was warmed before measurement; the longer-warmup repeat
restored normally and also showed no gain. The two temporary-filter rows use
the same APK, SHA-256
`E8AD3B8F40AE43B7CD28127F06E0D95B34D15A6E084E663751EBD9EDFE96EA43`.
Their 0.07 fps difference (0.4%) does not justify retaining the filter.

Matching headless replays confirmed that all 305 guest SPIR-V modules changed
with global optimization, reducing their total size from 18,600,472 to
15,019,208 bytes. Adreno instruction counts did not improve consistently;
the large vertex shader grew from 43,355 to 43,947 instructions. A temporary
filter isolated 12 pixel shaders with reduced driver-reported scratch usage.
Exactly those 12 modules changed, matched their globally optimized versions,
and the other 293 remained byte-identical to baseline. A new-build disabled
control matched all 305 original modules. Early live startup capture also
confirmed both filter flags were applied and 12 optimizations succeeded.

The 38-resolve optimized and filtered replays matched each other exactly.
They did not consistently match the disabled control: final RGB output
differed in 109 of 829,440 pixels, maximum channel delta 12, while repeating
the disabled control differed in 102 pixels with the same maximum delta.
This small replay variability remains unexplained and does not establish
image equivalence. Host SPIR-V validation rejected 208 modules in both the
disabled and globally optimized dumps with existing `OpSelect` and
`OpLoopMerge` errors; no previously valid module became invalid.

The temporary filter was removed, the original committed APK was restored,
and global optimization remains disabled. Evidence is retained in the
artifacts workspace's `diagnostics-spirv-opt-20260930` directory. After the
probe, all 32 protected private files and both external configuration files
were restored or verified against fresh backups, and all seven diagnostic
slot hashes were verified unchanged.

### Repeatable gameplay measurement

The bundled profile uses `readback_resolve=fast` and asynchronous shader
compilation, and leaves memory budgets to the native Android defaults. It is
copied only on first launch. Updating the APK preserves existing internal and
external profiles; it does not migrate their settings. Back up both profiles
and `xenios.config.toml` before adjusting an existing install. The September 26
Odin tests already used fast readback, so the bundled-profile correction is
not a newly measured speedup on that device.

`tools/android/Measure-AndroidGameplay.ps1` measures an already-running scene
without installing, launching, restoring, clearing logs or writing device
configuration. Restore the same diagnostic scene, let shaders finish compiling,
confirm input and new game frames, then collect at least two minutes:

```powershell
.\tools\android\Measure-AndroidGameplay.ps1 -Serial adf63ecd -Seconds 120 -OutputRoot C:\captures\xenios-perf
```

Each unique bundle contains the installed APK(s) and SHA-256 hashes, both
profile locations, saved config, startup and continuous logs, GPU clock/busy
samples, memory information and a final screenshot. `summary.json` reports
elapsed-time-weighted swap and refreshed-output FPS, frame-weighted GPU time
when timestamps are enabled, and intervals with pending shader compilation.
The first summary interval is discarded because it may precede collection.
A process change or guest frame-counter reset rejects the measurement.
A refreshed output and screenshot still need a visual/input check; they do
not establish image accuracy or general game compatibility.

Enable `vulkan_gpu_timing=true` in a backed-up profile for a profiling run;
it remains off in the bundled gameplay profile. Use the same diagnostics in
both sides of an A/B comparison. The collector can summarize an existing log
without a device, and has a host-only accounting check:

```powershell
.\tools\android\Measure-AndroidGameplay.ps1 -LogPath C:\captures\run\logcat.txt
.\tools\android\Measure-AndroidGameplay.ps1 -SelfTest
```

## Trace diff: finding rendering bugs

Don't tune workarounds by eye. Record the bad frame on the phone, replay the
same frame with Canary's D3D12/Vulkan trace dump on the PC (the reference) and
with this tree's Vulkan trace dump on the phone, then find the first resolve
and draw where they differ. Harness and scripts: `E:\xbox360emu-build\trace-harness`
(see its `README.md`). The Canary reference build is
`E:\xbox360emu-build\xenia-canary-src`.

- Record: create `<external-files>/android_trace_frame.txt`; the next frame is
  written to `<external-files>/traces/` (list with `ls -t`).
  `capture_when.ps1` triggers a trace and a save state automatically when the
  screen shows a defect (for example magenta pixels).
- Replay: `replay-windows.ps1` (pass `--async_shader_compilation=false`, or
  D3D12 skips draws) and `replay-android.ps1`.
- Compare: `compare.py resolves`, `channel_stats.py` (per-channel means
  per resolve; a tint shows as channel drift), `decode_resolves.py` (images).
- Narrow down: `bisect_patches.ps1` (which Halo patch changes a resolve) and
  `bisect_draw.ps1` (first draw whose EDRAM tiles diverge, via
  `--trace_dump_edram_draws`). `--trace_dump_skip_draw_after_setup=N`
  separates render-target setup (transfers) from the draw itself.
  `log_ownership_changes=1` with `ownership_watch_start_tiles` and
  `ownership_watch_end_tiles` in `halo_experiment.txt` logs ownership
  transfers.
- Caveats: depth words differ by a few ULP between backends (compare depth
  as 24-bit values, not as colour channels). Canary D3D12 and Canary Vulkan
  also differ on some MSAA resolves, so use the Canary Vulkan run as a second
  reference.

## Documentation maintenance

Keep current facts here and link to bounded test evidence. Label hypotheses
and untested paths. Change status only after the relevant behavior is observed.
Do not infer compatibility from an APK build, report number or prior AI claim.
Keep historical designs under `docs/archive/`; the original save-state design
describes a different container contract from the current diagnostic slot.
