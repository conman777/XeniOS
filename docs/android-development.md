# Android development

Current status: **experimental; Halo Reach is not playable on the tested
Odin2 Portal**. Menus and input work. The campaign has severe visual corruption
and long stalls. A successful build or a visible menu does not establish game
compatibility.

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

## Documentation maintenance

Keep current facts here and link to bounded test evidence. Label hypotheses
and untested paths. Change status only after the relevant behavior is observed.
Do not infer compatibility from an APK build, report number or prior AI claim.
Keep historical designs under `docs/archive/`; the original save-state design
describes a different container contract from the current diagnostic slot.
