# Android live diagnostic capture and save/restore

See the [Android development guide](android-development.md) for the current
device status and build procedure. This feature remains experimental.

This debug-only workflow is intentionally user-triggered. It does not navigate
a game, press controller buttons, or select a RenderDoc target. The
`Capture diagnostic report` button creates a host-collector marker without
saving emulator state. `Capture + save state` creates both the marker and one
checksummed diagnostic emulator slot.

## Live save/restore

1. Install a GitHub debug APK, launch a title and open **Tools**. The orange
   `DEBUG DIAGNOSTICS — NOT GAMEPLAY UI` panel is excluded from release builds.
2. Press `Capture + save state` and keep XeniOS open until the operation
   completes. A large title may take several minutes while guest memory is
   serialized; the panel displays elapsed time while the operation is live.
3. A successful capture publishes
   `<external-files>/diagnostic_save/current.xes`. The completed slot replaces
   the previous slot only after serialization, hashing, flushing, and file
   truncation finish.
4. Press `Restore saved state`, review the confirmation, and choose OK. Restore
   validates the signature, format version, payload length, XXH3-64 checksum,
   and title ID before pausing or mutating the running title.

Capture attempts to cooperatively park guest CPU execution and its registered
live producers:
audio/XMA, input, kernel asynchronous work, VFS mutations, GPU command ingress,
the command worker, Vulkan graphics and sparse queues, readback/completion
paths, presenter/mailbox work, and the vblank/interrupt worker. After a
successful save, each producer is reopened and the title resumes.

Restore reconstructs serialized CPU state, GPU registers/EDRAM, audio client
registrations, kernel objects/threads and guest memory. This is not a complete
serialization of all host decoder or render-target cache state. The tested
Halo scene returned after an app restart, but compatibility across builds,
configurations, game revisions and devices is not established. Version 6 does
not bind the slot to a process session or hash all those identities.

Validation and safe-point failures leave the existing title running. A checked
Vulkan upload failure after destructive restore starts is reported and leaves
emulation paused. Restart the app to recover. A success message must be
followed by a check that new game frames and input actually work.

## Operator checklist

1. Install a debug APK. The orange diagnostics panel must be absent from
   release builds.
2. Start the host collector and leave it waiting:

   ```powershell
   .\tools\android\Capture-XeniOSDiagnostics.ps1 `
     -Serial adf63ecd `
     -OutputRoot C:\captures\xenios
   ```

3. Navigate normally with the controller. When the corrupt frame is visibly
   present, press `Capture diagnostic report` if no save is needed, or
   `Capture + save state` if a restorable emulator slot is also required.
4. Wait for the in-app confirmation containing the request ID. Keep the scene
   visible until the host reports that the bundle is complete.
5. Confirm the bundle contains `android-screen.png`, `logcat-bounded.txt`,
   process/memory state, `app-request\request.json`,
   `app-request\app_runtime.json`, and
   `app-request\native_snapshot.json`.

The report-only marker is written immediately. For a combined capture, the
marker and app-side metadata are written only after the emulator slot succeeds.
The request records whether it created a save and includes the guest target,
title ID/name once XeniOS has exposed them,
requested and selected Vulkan render-target paths, fragment-interlock support,
latest resolve fields, Halo compatibility ownership state, active profile
copies, app/runtime memory metadata, and the save-slot path. XeniOS does not
currently expose a general guest map name, so the snapshot records that field
as unavailable rather than guessing.

## RenderDoc prerequisite

The app button cannot guarantee a Vulkan RDC. It creates a correlated marker
only.

Before allowing the collector to request one capture, independently verify in
QRenderDoc target control that:

- the selected process is the XeniOS debug package;
- the producer API is Vulkan, not the Android OpenGLES presentation layer; and
- a prior inspection shows XeniOS Vulkan command buffers/resources rather than
  presentation-only work.

After that verification, provide a local single-shot trigger adapter:

```powershell
.\tools\android\Capture-XeniOSDiagnostics.ps1 `
  -Serial adf63ecd `
  -OutputRoot C:\captures\xenios `
  -RenderDocTargetVerified `
  -RenderDocTriggerScript C:\tools\Trigger-VerifiedXeniOSRenderDoc.ps1
```

The adapter is invoked exactly once with `-Serial`, `-Package`, `-RequestId`,
and `-OutputDirectory`. Without both the explicit verification switch and the
adapter, the collector refuses or skips RenderDoc and records why in
`renderdoc-status.txt`.

## Normal-game checkpoint policy

Live diagnostic Save/Restore is separate from Halo Reach's normal guest
checkpoint files. It does not synthesize or edit an in-game checkpoint.

If a reusable Halo checkpoint is wanted:

1. The user reaches a normal in-game checkpoint and waits for the autosave
   indicator and disk activity to finish.
2. The user verifies the checkpoint is stable through normal game behavior.
3. Only after explicit user approval, stop XeniOS and make a host-side,
   read-only copy of the existing save/profile files.
4. Record source paths, timestamps, sizes, and hashes with the copy.
5. Never push the copy back, rename, truncate, or otherwise modify device saves
   as part of diagnostic collection.

Checkpoint copying remains deliberately separate from the live emulator slot
and `Capture-XeniOSDiagnostics.ps1`.

## Tests

Run the source-only checks without a device:

```powershell
cd android\android_studio_project
.\gradlew.bat testGithubDebugUnitTest

powershell -NoProfile -ExecutionPolicy Bypass `
  -File ..\..\tools\android\Capture-XeniOSDiagnostics.ps1 -SelfTest
```
