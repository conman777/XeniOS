# Android development

Current status (2026-09-24): **experimental; Halo Reach reaches campaign
gameplay on the tested Odin2 Portal but renders incorrectly**. Menus, input,
the intro cinematic and first-person gameplay with HUD run at 6-18 fps
(optimized native build). Grass, sky and some characters render magenta, and
there are frame-rate dips while new shaders compile. A successful build or a
visible menu does not establish game compatibility.

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
(intro valley shot) and `pink_grass.xes` (Noble Team leaving the hangar,
magenta grass/sky). Don't run trace replays while the app is running: the
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
(see [Trace diff](#trace-diff-finding-rendering-bugs)). All changes are uncommitted
in the working tree unless the git log says otherwise.

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

Identified, not yet fixed:

- **Yellow glowing foliage** is caused by
  `halo_android_compat_presentable_color_shadow` (default on). Of all 13 Halo
  patches (9 `halo_experiment.txt` keys plus 4 cvars), it is the only one that
  breaks the affected buffer. With it off (profile key
  `halo_android_compat_presentable_color_shadow=false`) the glow is gone live.
  The phone profile currently has it off.
- **Magenta grass, sky and characters** remain with every patch off. The
  lighting/HDR (7e3, EDRAM tile 675) buffer diverges from the reference. It is
  not the SNORM16 fallback (Adreno supports SNORM16 attachments) and not
  shader structurization.
- **Replays must use the app's configuration.** The trace dump doesn't apply
  the cvar overrides the app forces on Android (`xenia_main.cc`: dynamic
  rendering off, `tiled_shared_memory`, memory limits). Use
  `replay-android.ps1 -AppConfig`. The earlier "draw 280 / draw 171" whole-
  buffer corruption was `vulkan_dynamic_rendering=true`, which only the trace
  dump used. With dynamic rendering, the MRT draw wrote its second output
  (normals) into RT0. The app is unaffected.
- **Lit scene missing (dark / wrongly lit frames), located:** in `pink_grass`
  with `-AppConfig`, draw 1196 is a full-screen depth-linearization pass. It
  point-samples the 1152x720 k_8_8_8_8 texture at 0x02D08000 (resolve 6) and
  writes `1/(d*c100.y+c100.x)` into the 7e3 buffer at EDRAM tile 675. The
  phone writes ~0 almost everywhere (97.7% zero pixels against 45.8% on D3D12
  and Canary Vulkan), so all later lighting is missing. Ruled out: the input
  bytes (resolve 6 matches), the binding (key, format, swizzle 0x60A),
  stale textures (`vulkan_debug_clear_textures_after_resolve`), the fork's
  host color clamp (`spirv_host_color_clamp`), `vulkan_precise_interpolation`,
  and all Halo patches. Narrowed further with raw host dumps
  (`--trace_dump_texture_slot`, `--trace_dump_color0_host`) and shader output
  replacement (`--spirv_debug_ps_hash`, `--spirv_debug_ps_output`, where -100
  means a literal):
  - The sampled texture's host contents match the guest bytes exactly.
  - `c100` is sane: (0.0001, 128).
  - With the draw skipped, the float16 RT0 keeps the transferred contents.
  - With the draw, RT0 is all 0.0, even when the shader outputs a literal
    (1,2,3,4).
  - No GPU fault appears in logcat or dmesg.

  So the draw's pixels never reach this image, and the image is wiped. The
  suspects are the framebuffer or image view bound for this RT key (7e3
  "AS_16_16_16_16", EDRAM tile 675, 1200x2192) and render-pass behaviour on
  Adreno. The next step is the Vulkan validation layer on the device.
- **`writer_gb_fix`** (under `direct_presentable_resolve`) swaps two channels
  of the final resolve. The present swizzle override `0xA42` appears to
  compensate for it. Left unchanged.

Performance: optimized native build 6-18 fps in the intro cinematic and
mission, against 3.5-12.5 unoptimized. The dips coincide with pipeline
creation backlog (`SwapSummary ... queued=`). KGSL peaks at about 4.1 GB
against the profile's `vulkan_kgsl_memory_limit_mb=3584`, so a pipeline trim
still occurs occasionally. A 4608 MB profile
(`trace-harness\configs\internal_profile.kgsl4608.txt`) is untested.

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
