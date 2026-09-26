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
