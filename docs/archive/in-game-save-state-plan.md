# Historical design

This is an archived proposal, not the current diagnostic slot contract.
See [Android development](../android-development.md) for implemented and tested behavior.

---

# In-game same-process save-state plan

## Contract

The debug control will request a save or restore at the next global quiescent
boundary. It will not capture an arbitrary CPU instruction, an in-flight Vulkan
command, or opaque driver state. A restore targets equivalent guest-visible
state after host caches are rebuilt, not bit-identical renderer timing.

The first supported product is one same-process slot for one running title.
Every container is bound to a random process-session identifier, title, game
image, build, configuration, renderer backend, and device identity. A state
from another process or incompatible configuration must be rejected before any
live state is changed.

The normal `cache1:\autosave` checkpoint feature is separate. Copying it while
the game runs is never an emulator save state.

## Phase 1: contracts and lifecycle

- Define a versioned, bounded container directory with SHA-256 identities.
- Require Identity, Clock, CPU, Memory, Kernel, GPU, Audio, Input, and VFS
  sections. Unknown required sections are rejected.
- Define a serialized operation state machine and a quiescence provider
  contract.
- Preserve the last verified slot until a replacement is fully checksummed and
  atomically published.
- Provide no Android control and call no legacy `SaveToFile` or
  `RestoreFromFile` path yet.

Exit criteria:

- Format round-trip, missing-section, overlap, stale-request, concurrent
  operation, previous-slot preservation, and boundary-release tests pass.
- Android native targets compile with the new core files.
- No runtime path can claim a save or restore succeeded.

## Phase 2: cooperative boundary, CPU, clock, and memory

- Add an A64 JIT cooperative basic-block safe-point barrier for all guest
  threads without POSIX stack unwinding.
- Freeze guest clock/timestamps and all guest-visible mutation.
- Serialize the complete PPC architectural context, including MSR, VSCR,
  VRSAVE, reservation state, APC/wait state, and safe resume PC.
- Serialize allocation metadata and committed memory pages with bounded
  sections and compression.
- Invalidate or rebuild derived A64 code cache state after restore.

Foundation delivered:

- `CooperativeGuestBarrier` defines participant snapshots, generation-checked
  polling, bounded wait, held-state release, and timeout/cancellation rollback.
- Canonical, bounded CPU, clock, and memory records use fixed little-endian
  encodings. A64 reservation ownership and physical-memory aliases are explicit
  rather than inferred from host pointers.
- Fail-closed provider interfaces reject capture and restore until complete
  live subsystem adapters are supplied.

Live CPU integration delivered:

- `LiveGuestRuntime` enrolls guest-capable threads before their first execution,
  freezes enrollment during a boundary, retains stable context bindings in an
  RAII lease, and unfreezes creation/destruction after release or timeout.
- Generated A64 code performs a cheap generation check at the first
  `SourceOffset` of each HIR basic block, after `last_guest_pc` is updated. The
  slow poll revalidates the generation and parks cooperatively.
- The A64 CPU capture adapter copies the synchronized PPC architectural context
  and mutable NJM mode into a canonical CPU record. It rejects active
  reservations, pending stack synchronization, inconsistent derived FP state,
  and invalid resume PCs.
- CPU records now include bounded, deepest-to-outermost A64 stackpoint
  metadata: guest stack pointer, guest return address, and captured generated
  host-stack size. Opaque host SP/FP values are deliberately excluded. The CPU
  wire format validates and round-trips all three fields.
- A pure resume planner validates the captured PC and converts that stackpoint
  chain into deterministic guest entry segments.
- Generated frames test a host-only unwind request after a held safe-point and
  after returning from non-tail generated calls. This supplies a compile-tested
  outward propagation path to each generated epilog without POSIX unwinding.
- Nothing arms that request or dispatches the resume segments yet. CPU restore
  therefore remains unsupported: the runtime owner, code-cache resolution, and
  verified frame re-entry sequence must be connected before any architectural
  context is overwritten.

Clock foundation delivered:

- Guest interrupt time is derived from restorable guest-tick and 100 ns time
  anchors rather than host uptime. The conversion is deterministic, bounded,
  and saturating.
- The clock can be frozen, captured while frozen, restored only while frozen,
  and unfrozen with a fresh host-tick anchor so serialization latency does not
  advance guest time.
- The transactional operation runner does not yet coordinate this boundary
  with kernel timers or other subsystem-specific host deadlines.

Memory staging foundation delivered:

- `Memory` can enumerate allocated page metadata across every virtual heap,
  the three physical alias heaps, and the physical heap under one shared global
  lock. Enumeration records physical backing identities without reading or
  changing page contents.
- A mutation-free preflight planner validates and canonicalizes the target and
  current inventory, then produces ordered create/change/release transitions.
- Bounded content capture copies committed standalone virtual and physical
  pages through a read-only page source. Physical aliases carry backing
  identities instead of duplicate bytes. Capture builds privately, enforces
  page and byte limits, validates the complete canonical memory format, and
  leaves the caller's previous snapshot unchanged on any read or validation
  failure.
- There is intentionally no live `Memory` transition apply API. Page contents,
  host mapping and protection staging, alias consistency, rollback, and
  participation by non-CPU memory writers are required first.

Transactional restore foundation delivered:

- Subsystem participants have explicit `Prepare`, `Commit`, `Rollback`, and
  infallible `Finalize` contracts. All preparation completes before mutation;
  commit, rollback, and finalization run in reverse registration order.
- Registering the frozen-clock participant before planned memory keeps guest
  time frozen while memory commits or rolls back. A clock commit failure after
  memory mutation rolls memory back before the original clock is restored and
  unfrozen.
- The planned-memory participant captures current allocation metadata, builds
  the immutable transition plan, and requires detached rollback staging before
  commit. Failure-injection tests prove coordinator ordering and restoration
  using a detached test backend.
- A bounded detached-memory backend validates and canonicalizes the target in
  private storage, verifies the supplied transition plan, commits with a
  no-allocation vector swap, and rolls back with the inverse swap. Exact encoded
  pre-state equality is tested after a downstream clock failure.
- A content-only live-memory prototype now exists for the strictly narrower
  unchanged-topology case. It is not registered as a restore participant or
  called by any save-state path. Mapping, protection, allocation, and alias
  changes remain unsupported.

Cross-subsystem quiescence contract delivered:

- A non-invasive coordinator now requires exactly one participant for kernel
  dispatch/timers, VFS/I/O, Vulkan command processing/submissions, audio, and
  input. It prepares them in that fixed order and owns the resulting boundaries
  in one move-only lease. No production subsystem implements the participant
  interface yet.
- The order first closes the kernel delivery consumer and then its VFS/I/O
  producer before pausing independent GPU, audio, and input producers. Abort
  and normal release run in exact reverse order, reopening producers before
  kernel delivery. The previously established guest-thread boundary would
  surround this lease and release last, but it remains deliberately unwired.
- All participants share one absolute bounded deadline. `Prepare` may only
  establish a reversible admission gate and drain already accepted work; it
  must undo its own partial work if it fails. A late reported success is
  immediately included in reverse abort and the whole acquisition fails
  closed. The coordinator cannot forcibly interrupt a broken participant, so
  every production adapter must itself provide deadline-aware waits.
- Fake-only tests cover successful ownership, every participant failure,
  missing/duplicated/out-of-order registration, late success, reverse abort,
  and sole ownership after move/destruction. These tests do not call any live
  subsystem, serialize state, or register CPU or memory restore.

Existing primitives and exact unsupported cases:

- Kernel dispatch/timers: dispatch queue admission is protected by the kernel
  global lock, but dequeued callbacks execute after that lock is released.
  Deferred and immediate completions, APC/DPC delivery, `XTimer`, the repeating
  timestamp timer, and host waits have no common reversible gate, in-flight
  count, or bounded acknowledgement. `ShutdownDispatchThread` is destructive
  lifecycle control and is not a quiescence primitive.
- VFS/I/O: the VFS global lock protects device and symlink namespaces, while
  individual `XFile` operations use their own locks and may signal events or
  enqueue kernel completion work. There is no central admission gate or drain
  spanning open devices/files, mapped host writes, deferred completions, and
  backend I/O. A future adapter must coordinate with the kernel participant
  rather than treating the namespace lock as an I/O barrier.
- Vulkan: `CommandProcessor::Pause` can reach and suspend its worker, and the
  Vulkan processor can end a submission and wait for queue completion.
  Currently those operations are not one atomic, deadline-bounded boundary:
  pending worker functions, readbacks, queries, memexports, presenter work, and
  device-loss failure must be accounted for before acknowledgement.
- Audio: `AudioSystem::Pause` and `XmaDecoder::Pause` acknowledge their worker
  threads, but their waits are not deadline-aware and do not prove that every
  driver queue, backend callback, media-player path, guest callback, semaphore,
  and guest-visible counter is stable.
- Input: the UI input blocker masks selected game reads only. Driver polling,
  slot/packet/keystroke state, notifications, vibration, and blocker-removal
  bookkeeping may still change. Input has no reversible pause/drain
  acknowledgement suitable for save-state ownership.

First real admission gates delivered:

- `ReversibleAdmissionGate` provides one mutex-protected admission boundary,
  move-only in-flight leases, a shared steady-clock deadline, exact owner
  identity, and reversible close/drain/reopen behavior. New operations wait
  while closed. A close timeout automatically reopens and wakes them; a wrong
  owner cannot reopen the gate.
- `InputSystem` now admits all calls it owns that can poll or mutate driver
  state through one gate: capabilities, game and UI state, keystrokes,
  connection bookkeeping, UI blocker transitions, vibration writes/toggles,
  driver registration, and Skylander portal reads/writes. Private ungated
  helpers keep compound operations under one lease and avoid nested-gate
  deadlocks.
- The input gate is not registered as the input quiescence participant.
  Driver-internal event threads and opaque host vibration state have no
  cooperative acknowledgement yet. Closing the gate proves only that
  `InputSystem`-owned calls have drained and no later such call can enter.
- `KernelState` now admits deferred dispatch enqueue and callback execution,
  immediate/deferred overlapped completion mutation, and timestamp bundle
  updates through one kernel dispatch/timer gate. The dispatch worker obtains
  its lease before removing a queued callback, so a closed gate leaves pending
  work intact. `XTimer` set/cancel and timer-fired APC callbacks use the same
  gate.
- The kernel gate is not registered as the kernel participant. Direct APC
  enqueue sites, DPCs, notification delivery, host waits, and other kernel
  object mutation remain outside it. The host waitable timer may also become
  signaled or advance its period before an `XTimer` callback reaches the gate,
  and timers without APC callbacks have no gated firing hook. `XTimer` and the
  timestamp updater run on shared host timer threads; closed-gate callbacks
  wait and therefore require a tightly bounded future owner/lifecycle protocol
  before live use.
- Deterministic host tests hold fake polling/callback leases, close from a
  second thread, prove later vibration/timer work cannot enter, drain accepted
  work, and verify exact owner reopen. Separate tests prove timeout rollback,
  no stranded callbacks, conflicting-owner rejection, and move-only
  accounting.

Bounded VFS/I/O admission delivered:

- `VirtualFileSystem` owns a separate reversible guest-write gate with the same
  absolute-deadline, owner, drain, timeout rollback, and reverse-open semantics
  as the other admission boundaries. It is not registered as the VFS/I/O
  participant.
- The synchronous `NtWriteFile` path acquires kernel completion admission
  first and VFS write admission second. Both leases remain held across the
  backend write, entry metadata refresh, completion-port notification, guest
  I/O status update, APC enqueue, and final event signal. This ensures the
  kernel boundary cannot acknowledge while an admitted file write still has a
  completion to publish.
- Mutating `NtCreateFile` dispositions are held through possible
  delete/create/open work and the guest I/O status result. VFS-mutating
  `NtSetInformationFile` classes cover host attributes/timestamps, rename,
  delete-on-close intent, allocation length, and end-of-file length.
  Delete-on-close destruction acquires kernel completion ownership followed by
  VFS ownership through its event signal and backend destruction.
- Deterministic two-gate host tests prove exact write/completion ordering,
  kernel-then-VFS close acknowledgement, reverse reopen, and that no later
  backend write is admitted after closure. A separate held-write test proves a
  close timeout automatically reopens and does not strand existing or later
  writers.
- This boundary does not cover direct emulator-internal
  `vfs::File::WriteSync` or `SetLength` calls, direct
  `VirtualFileSystem::CreatePath`/`DeletePath`/`OpenFile` mutation, writable
  mapped files, the unimplemented backend async APIs, device-specific worker
  I/O, content/profile code using host filesystem handles, extraction/header
  helpers, or writes made by external host processes.
- Plain `NtCreateFile(kOpen)` is not classified as a writer even though its
  stale host-entry cleanup may remove cached VFS metadata. `NtFlushBuffersFile`
  remains a stub. Host mapping lifetime, dirty-page writeback, filesystem cache
  flush, and backend cancellation have no common acknowledgement. These
  bypasses must join the gate before a complete VFS/I/O participant can report
  success.

Detached immutable VFS-generation foundation delivered:

- `DetachedVfsGenerationStore` is an unregistered, same-process model for
  managed writable mounts. Mount and file nodes are immutable; saved snapshots
  share those roots, while a write clones only the target mount node and target
  file contents. Store creation and restore staging deep-copy caller-owned
  roots so a retained mutable alias cannot alter a saved generation.
- Every mount, generation, file, and open handle has a nonzero stable identity.
  Open-file metadata records the matching mount/file generation, canonical
  guest path, byte position, access, and synchronous mode. Restore requires the
  live mount set and open-handle identity/configuration set to match exactly;
  only generation roots and positions may change.
- Restore staging validates and owns a complete detached target before taking
  transaction ownership. Commit swaps the entire mount-root graph and
  open-file metadata under one mutex using no-allocation vector swaps.
  Rollback performs the inverse swaps exactly; destruction automatically rolls
  back an unfinalized commit. Competing capture, write, position, or restore
  requests return busy instead of waiting behind an unbounded transaction.
- Counts, paths, per-file bytes, total bytes, generation overflow, identity
  ordering, duplicate paths, and open-file references are bounded and
  validated before mutation. Writes beyond limits and topology-changing
  restores leave the current root unchanged.
- The prototype explicitly rejects direct host-path, mapped-host, async-host,
  and raw-host mount backing. It also rejects asynchronous, mapped, or raw open
  files, directory enumeration, delete-on-close, pending I/O, and completion
  ports. It does not serialize a VFS section, create a host overlay, reopen or
  switch a live file, flush a host cache, or register a VFS participant.
- Live use requires a generation-managed overlay for every writable guest
  mount whose root switch is truly atomic, stable `XFile`/entry identities,
  captured directory enumeration and share/lock state, all open/close and
  metadata mutations behind the common boundary, and cancellation/drain
  contracts for mapped, async, raw-handle, device-worker, and external-host
  writers. Unsupported paths must continue to reject the save request.

Vulkan/GPU quiescence foundation delivered:

- `GpuQuiescenceBoundary` is an unregistered, backend-independent transaction
  foundation. Producers accepted before closure retain move-only leases; a
  request closes admission, drains those leases under one absolute deadline,
  and only then delegates to a command-processor/backend control lane.
- A successful request returns one move-only `HeldGpuQuiescence`. Release
  aborts the backend hold before reopening producer admission. Backend failure,
  unsupported state, or a success reported after the deadline invokes
  idempotent rollback and reopens admission. Deterministic fakes cover accepted
  work draining, later producer blocking, admission timeout, partial backend
  failure, late success, exclusive ownership, and move-only release.
- No Vulkan participant is registered. The abstract boundary serializes no GPU
  state and does not call the legacy command-processor save/restore path.
- `CommandProcessor` now owns a separate, actual two-lane
  `GpuCommandAdmissionLanes` foundation. `UpdateWritePointer` retains a PM4
  producer lease from write-pointer publication until the worker reaches that
  pointer; the visible `CP_RB_WPTR` register is updated inside the same admitted
  publication. Ring initialization and read-pointer writeback configuration
  are admitted mutations. If a write pointer already equals the read pointer,
  no false pending lease is retained.
- `CallInThread` retains a callback lease from enqueue through worker
  execution, including callbacks queued from non-worker threads. Closing the
  two lanes uses one absolute deadline. PM4 closes first; callback timeout or
  failure reopens PM4 automatically. Later producers block until exact-owner
  reopen, and shutdown releases any abandoned PM4 or callback leases.
- The same foundation now has a reversible, owner-bound worker control lane.
  `ParkWorkerAndWait` first closes and drains both ingress lanes, publishes an
  out-of-band park request, wakes the worker event, and waits only until the
  caller's absolute deadline for acknowledgement at the top of the worker
  loop. The low-power loop observes the request instead of waiting through
  repeated sleep intervals. A timeout cancels the request and reopens both
  ingress lanes; a shutdown cancellation wakes a parked worker before join.
- Release is also deadline-aware and ordered: the exact owner requests worker
  resume, waits for the worker to acknowledge leaving the park, and only then
  reopens callbacks and PM4. A stale owner changes nothing. If resume
  acknowledgement times out, both ingress lanes remain closed and ownership is
  retained so release can be retried without admitting work into an uncertain
  worker state.
- Deterministic host tests cover active PM4 drain timeout, queued callback
  ownership through execution, successful worker acknowledgement, stale-owner
  rejection, acknowledgement timeout rollback/reopen, and later producer
  admission. The focused protocol passes strict host compilation and the real
  `CommandProcessor` safe-loop integration passes Android A64 NDK syntax
  compilation.
- These park/resume methods are not called by a save participant. A successful
  park proves only that admitted PM4 parsing and pending command-processor
  callbacks are complete and that the CPU worker is stopped between loop
  iterations. It does not complete backend work already emitted while parsing
  PM4.
- `GpuSubmissionLifecycleCoordinator` is a separate, unregistered two-phase
  preflight contract for the next Vulkan layer. Preparation may acquire only
  reversible logical ownership and return a read-only snapshot; it must prove
  draw ingress closure, execution on the parked worker control lane, closure of
  independent submission producers, nonblocking completion polling, consistent
  submission indices, a valid active command buffer, preallocated close
  resources, and failure-atomic close/submit behavior. Pending sparse binds or
  wait semaphores from an earlier queue mutation are rejected explicitly.
- Commit is allowed only after that proof. A backend failure is reversible only
  when it certifies that no semantic or Vulkan mutation occurred. Once a
  submission may have changed, an indeterminate failure or completion timeout
  retains the owner and follows a fail-closed path; it never reopens draw or
  command ingress. Completion is acknowledged only through nonblocking polls
  under an absolute deadline. A pending hold can be polled again, and admission
  reopens only after the target submission is confirmed complete.
- Deterministic fakes cover successful active-submission commit, existing
  closed submissions, already-completed submissions, bounded timeout with
  later acknowledgement, rejection of the current non-atomic sparse-bind
  shape, reversible pre-mutation failure, indeterminate post-mutation failure,
  and invalid completion indices. There is no live Vulkan adapter or caller.
- `CommandProcessor::Pause` is not a usable quiescence boundary: it posts a
  worker callback and waits without a deadline. The new lanes deliberately do
  not call it. Suspending the worker also does not by itself drain Vulkan or
  presentation queues.
- `VulkanCommandProcessor::EndSubmission` may allocate command pools and
  semaphores and may fail while leaving the submission open. More importantly,
  it ends the render pass and subsystem submission state, flushes uniform
  writes, and may successfully execute `vkQueueBindSparse` before command-pool
  reset, command-buffer recording, fence acquisition, or graphics queue submit
  fails. `current_submission_wait_semaphores_` then represents an already
  mutated retry state. The existing routine therefore cannot honestly claim
  `close_submit_failure_atomic`.
  `AwaitAllQueueOperationsCompletion` reaches
  `GPUCompletionTimeline::AwaitSubmissionAndUpdateCompleted`, which has no
  deadline/cancellation contract; Vulkan ultimately calls `vkWaitForFences`
  with `UINT64_MAX`. Device loss also invokes a callback and mutates reclaim
  queues while completion is checked. Completion processing can additionally
  reclaim caches/resources and publish occlusion-query results to guest memory,
  so a future nonblocking poll adapter must join memory and kernel ownership
  rather than merely expose fence status.
- Resolve readbacks, double-buffered memexport readbacks, shared-memory GPU
  writes, and occlusion-query results become guest-visible at different
  submission points. Draining the graphics queue alone is insufficient until
  their pending ranges, mapped buffers, delayed submission indices, and guest
  memory publication are acknowledged under the same owner.
- The new PM4 lease ends after CPU command parsing and read-pointer writeback,
  before any proof that an active Vulkan submission, sparse bind, resolve
  readback, memexport copy, occlusion-query copy, or shared-memory download has
  completed. `readback_buffers_`, double-buffered
  `memexport_readback_buffers_`, their submission indices/ranges, the full-mode
  mapped memexport buffer, and the mapped occlusion-query buffer therefore
  remain explicitly unsupported.
- `Presenter::RefreshGuestOutput` publishes a three-image mailbox from the GPU
  thread, while `VulkanPresenter::PaintAndPresentImpl` consumes it and submits
  or presents from the UI thread. Guest-output refresh, UI paint, and present
  use separate completion timelines; swapchain retirement also calls
  unbounded timeline waits and `vkQueueWaitIdle`.
- Direct frame-trace state changes, shader-storage completion paths invoked
  outside `CallInThread`, GPU interrupt delivery, device-loss callbacks, and
  any backend or UI producer that does not enter these two command lanes also
  remain outside the boundary.
- Submission lifecycle inspection and close must execute on the Vulkan command
  worker. The current base-class park request acknowledges and sleeps without a
  derived-backend pre-park callback, so invoking `EndSubmission` from a
  coordinator thread would violate ownership. A future adapter needs a bounded
  worker-side prepare/commit callback before park acknowledgement, with
  preallocated resources and a failure-atomic queue transition; this phase does
  not add that hook.
- A live adapter must therefore compose the bounded ingress/worker park already
  delivered here with bounded, reversible ownership for active submission
  close, graphics/sparse queue completion, every readback publication,
  presenter refresh admission, UI paint admission, and presentation-queue
  completion. It must prove that abort never enters the closed producer gate
  and must not use `vkDeviceWaitIdle`, `vkQueueWaitIdle`, or another unbounded
  wait as a substitute. Until all of these are owned atomically, the honest
  adapter result is `kUnsupported`.

Audio quiescence foundation and semantic inventory delivered:

- `AudioQuiescenceBoundary` is an unregistered four-domain transaction:
  AudioSystem mutation/submission, XMA mutation/work, host buffer completion,
  and guest callback execution. Each domain uses move-only admissions and the
  same absolute deadline.
- Acquisition first closes AudioSystem and XMA admission. A backend control
  lane must then reversibly stop host playback consumption and drain queued
  buffers while completion callbacks remain admitted. Host completion
  admission closes next. A second backend acknowledgement must drain client
  semaphore tokens and guest callbacks and park the AudioSystem worker before
  guest callback admission closes.
- Failure, unsupported state, admission timeout, or a backend success reported
  after the deadline aborts backend holds and reopens every reached gate in
  reverse order. The backend abort contract forbids audio submission, semaphore
  release, guest execution, or entry through a closed gate. Deterministic fakes
  cover all four drain boundaries, partial backend failure, both timeout
  stages, late success, exclusive ownership, and move-only release.
- No AudioSystem, XMA, driver, or media-player path enters these gates yet, and
  no audio participant is registered. This phase captures and restores no
  audio data and does not call the legacy `AudioSystem::Save` or `Restore`.

Semantic emulated audio state that a later serializer must inventory:

- AudioSystem client slots: occupancy and stable index, guest callback and
  argument, wrapped guest pointer, driver format/binding, configured queue
  depth, client semaphore count, submitted/processed/dropped counters, and
  worker running/paused/wait/callback state. The existing legacy save path
  writes only callback metadata and does not lock the client array.
- XMA device state: the complete register file, context allocation bitmap,
  decoder implementation identity, all 320 guest `XMA_CONTEXT_DATA` records,
  enabled/allocated state, pending kicks and work-completion ownership, and the
  worker event/pause point.
- Each real XMA decoder context also owns opaque FFmpeg parser/codec packet,
  frame, bit-reservoir, overlap, and partial-frame state. Guest input/output
  buffers and their offsets live in guest memory, but that memory alone is not
  sufficient to reconstruct the host decoder at an arbitrary packet boundary.
- Guest callback state includes the exact client semaphore token already
  consumed, whether `Processor::Execute` has entered guest code, its enrolled
  CPU context/stack, and any callback-created kernel work. The common
  guest-thread and kernel boundaries must own this state; audio must not
  duplicate it.
- The independent `AudioMediaPlayer` adds playlist/song/state/volume, a
  separate worker, driver and semaphore, FFmpeg decode position, partially
  converted samples, and notification ownership. Its capture callback is
  currently disabled, but its host output remains an audio writer.

Host audio limitations blocking a live adapter:

- `AudioSystem::Pause` waits on an unbounded fence, may encounter a guest
  callback already executing, pauses XMA only, and does not pause or drain
  registered audio drivers. `XmaDecoder::Pause` waits only after a full
  320-context scan and has no deadline; when dedicated XMA threading is
  disabled, its worker exits without providing that pause acknowledgement.
- XAudio2 retains emulator frame slots plus opaque source-voice buffers.
  `Stop` does not prove buffer completion, and flushing would discard playback
  and alter callback/semaphore counts. Voice callbacks run on host-owned
  threads with no in-flight accounting.
- SDL owns a mutex-protected emulator queue, but its device callback may
  already be copying a buffer. `SDL_PauseAudioDevice` provides no bounded
  acknowledgement here that the callback is outside the critical section or
  that device-side audio latency has drained.
- ALSA has emulator ring indices plus kernel/hardware PCM buffers. Its worker
  uses bounded polling but `snd_pcm_pause` support is device-dependent, while
  `snd_pcm_drop` is destructive. Partially written/resampled frames and the
  hardware playback cursor are not restorable semantic state.
- NOP drivers complete immediately and are individually bounded, but do not
  prove the other configured or independently created driver paths safe.
- A live adapter must add non-destructive, deadline-aware host callback
  admission/in-flight accounting, exact emulator queue snapshots, and worker
  safe-point acknowledgements for every active driver and media player. Any
  backend lacking those capabilities must return `kUnsupported`.

Kernel async admission and accounting foundation delivered:

- `KernelAsyncAdmissionBoundary` separates APC, DPC, notification, wait, and
  timer domains. Each domain has reversible admission, bounded drain, owner
  validation, timeout rollback, and a stable pending-item count. Accounting
  overflow or underflow is permanent and makes later close fail closed.
- Pending-state policy is explicit. A caller may retain a stable count only
  when a future serializer can represent it; otherwise close rejects non-empty
  state and reopens normal execution. No live save participant uses this
  boundary yet.
- Central APC insertion, removal, execution, and rundown paths now account for
  queued APCs. Guest DPC queue insertion/removal and the direct CP-interrupt DPC
  callback enter the DPC domain. The current guest DPC queue is never
  dispatched, so any queued DPC must remain unsupported.
- Notification listener registration/broadcast, listener queues, and
  I/O-completion queues enter the notification domain. Queue producers,
  consumers, and object destruction update exact pending counts. An untracked
  queue, including one introduced by the legacy restore path, produces an
  accounting error rather than being accepted.
- `XObject` waits, signal-and-wait, wait-multiple, `XThread::Delay`, and
  I/O-completion waits enter the wait domain. A separate inventory validator
  accepts only the central Event, Mutant, NotifyListener, Semaphore, and Thread
  representations; Timer, IOCompletion, and unknown wait objects fail closed.
  This inventory classification does not claim those accepted objects are
  serialized.
- `XTimer` set/cancel, destruction, and APC-bearing timer callback execution
  enter the timer domain as well as the existing dispatch/timer gate. Each
  successfully armed timer is counted until cancellation or destruction.
  Because the host timer exposes no safe armed-state query, an expired one-shot
  stays conservatively pending. Callback-less host timer signaling is still
  outside this boundary, so either condition rejects quiescence.
- Nested notification/APC work is intentionally conservative: if close races
  a callback that needs a nested admission while an outer lease is active,
  bounded close times out and automatically reopens. It never reports a false
  quiescent boundary.
- Remaining bypasses include indefinite or backend-owned host waits, complete
  timer host state and callback-less expiration, kernel event/semaphore/mutant
  mutation outside the inspected wait calls, asynchronous object lifetime
  changes, the host user-callback wake used after APC insertion, all
  pending-object serialization, and any direct host callback not routed
  through these central functions. These must be inventoried and gated before
  kernel quiescence can be registered.

Remaining integration gates:

- Guest threads blocked in host exports or kernel waits need a cooperative
  host-boundary protocol. Until then they correctly cause a bounded timeout
  rather than being omitted from a state.
- A64 needs a runtime-owned, preflighted dispatcher that resolves every planned
  guest segment, rebuilds backend stackpoints, arms unwind only after all
  subsystems can restore, and proves multi-thread failure rollback.
- Read-only A64 dispatch preflight now resolves every existing generated entry
  and stack-size record into private output before mutation. A code-cache
  lookup now examines only already-published functions under the cache lock; it
  never calls `ResolveFunction`, compiles code, or writes an indirection.
  Preflight rejects a frame if its current generated stack size differs from
  the captured size.
- The next preflight seam is now explicit and deliberately non-executable.
  `A64RestorePreflight` is the single move-only owner of the held guest-thread
  boundary, an exclusive bounded code-generation lease, and detached
  per-thread re-entry preparations. It validates exact thread membership and
  every resolved target before accepting any preparation. Destruction releases
  detached preparations, the code-generation lease, and guest threads in that
  order.
- Code lookup is part of `A64FrozenCodeGeneration`, preventing a caller from
  accidentally resolving a target outside the leased generation. Generation
  validation is repeated after target resolution and detached preparation so a
  stale or contract-breaking adapter fails closed.
- The re-entry dispatcher currently exposes only `PrepareDetached` with a
  const live-context identity. There is no arm, unwind, execute, context-write,
  or commit method. Failure-injection tests prove that a partial multi-thread
  preparation is destroyed, the generation lease is released, all parked
  threads resume, and their context storage is byte-for-byte unchanged.
- `A64CodeCache` now has a dedicated generation gate separate from its
  placement lock. `A64Assembler` acquires a publication lease before emission
  and retains it through placement, source-map cloning, stack-size visibility,
  function `Setup`, and indirection installation. A bounded exclusive freeze
  waits for active publications, blocks new ones, and ties lookup to one
  validated generation. Concurrency tests cover timeout behind an active
  publisher, publisher blocking behind a freeze, and stale-token rejection.
- `A64CodeCacheGenerationControl` is the production preflight adapter over
  that gate. It remains unwired from a live restore coordinator, so no CPU
  execution or unwind behavior changed.
- The metadata is now sufficient to describe guest stackpoints, but not to
  manufacture live host SP/FP frames. A controlled re-entry trampoline must
  create those frames while preserving call/return behavior before unwind can
  be armed.
- Active reservation capture/restore needs a shared reservation-bitmap adapter;
  per-thread cached pointers are not serializable ownership.
- Guest clock restore must be wired to the runner together with kernel timers
  and all guest-visible time sources.
- Live memory still needs transactional mapping, protection, and alias
  mechanisms. The restricted data-only prototype must remain unwired until
  GPU, kernel, and every other memory writer join the same quiescent boundary.
- Derived code-cache state needs invalidation and restore validation.

Minimal bounded live-memory transaction prototype:

- The prototype rejects every snapshot whose canonical
  allocation, protection, and physical-alias topology differs from the current
  `xe::Memory` inventory. Supporting creates, releases, protection changes, or
  alias remaps would introduce fallible host-VM operations after mutation and
  is outside this initial transaction.
- `Memory::PrepareSaveStateStablePageTransaction` acquires the private global
  allocation lock, enumerates topology once, requires exact equality, and
  accepts only committed pages that are already host-writable.
- The narrowly scoped transaction is created by `Memory` itself and owns the
  private lock until rollback or finalize, extending the existing read-only
  `CaptureSaveStateAllocationInventory` pattern. There is no external
  inventory-to-pointer race window.
- It stages two bounded detached buffers before mutation: canonical target bytes
  and an exact undo copy of every unique backing page that will be written.
  Physical data is copied once through the canonical physical backing; alias
  ranges are validation-only views and are never written independently.
- Commit consists only of prevalidated fixed-size copies to stable,
  writable backing addresses. Rollback performs the inverse copies from the
  complete undo buffer. No allocation, protection call, mapping change, vector
  growth, or callback is permitted in either path.
- Per-page failure injection now proves exact pre-image restoration for every
  partial-copy boundary. Duplicate backing identities and overlapping live
  ranges are rejected; canonical physical aliases carry no independent data
  copy. The prototype remains unwired until the shared quiescence owner
  demonstrably excludes every non-CPU writer. This limited strategy restores
  contents only; later topology restoration needs a separate host-VM
  shadow/address-space design with equally strong rollback proof.

Exit criteria:

- Deterministic CPU/memory save, advance, restore, and hash tests pass.
- Boundary timeout or failure resumes the original state unchanged.
- Repeated restore does not leak suspended threads or memory.

## Phase 3: kernel, VFS, audio, and input

- Version and repair kernel object serialization. Implement or explicitly
  reject every live object type; never silently skip one.
- Capture timers, waits, APCs, dispatch work, async I/O, and completion state.
- Add immutable copy-on-write generations for every writable guest mount.
  Restore switches generations; it never rewinds a live host file in place.
- Serialize emulated XMA/audio queues and callback state. Flush host playback
  before resuming restored audio.
- Serialize emulated controller packet state. Clear rumble and hold game input
  neutral until the save/restore control is released.

Exit criteria:

- Kernel object inventory is complete for Halo Reach campaign.
- VFS rollback and failure-injection tests preserve the base files.
- Audio/input resume tests produce no duplicated input or stale rumble.

## Phase 4: Vulkan semantic graphics state

- Close and submit the active command buffer, drain GPU-written shared memory,
  and wait for all relevant Vulkan submissions.
- Serialize the complete guest GPU register file, command processor state,
  queries, memexports, resolves, swap/presenter state, and synchronization
  values.
- Canonicalize all host render targets into full EDRAM, then read back and
  checksum EDRAM plus ownership/format metadata.
- On restore, discard later Vulkan submissions and invalidate render-target,
  texture, primitive, descriptor, pipeline, and transient caches. Upload EDRAM
  and rebuild derived resources from restored guest memory and registers.

Exit criteria:

- EDRAM, GPU register, shared-memory, and first-presented-frame hashes
  round-trip.
- Repeated Halo Reach restores survive cache pressure and device-loss checks.
- Documentation states that host timing and opaque driver state are not
  reproduced.

## Phase 5: transactional runner and Android debug UI

- Preflight the complete container, identity, lengths, and hashes before
  entering restore.
- Prepare every subsystem and retain all rollback material before the first
  commit. Keep the clock frozen until memory and later subsystem commits have
  finalized or rolled back.
- Write to a unique temporary file, flush data and directory metadata, verify,
  and atomically publish the new slot. Keep the previous verified slot until
  publication succeeds.
- Route asynchronous native status through the Android windowed-app context.
- Add adjacent `Save current state` and `Restore saved state` controls only in
  debug `EmulatorActivity`. Disable both during an operation, consume the
  triggering input, require restore confirmation, and show durable success or
  failure text.

Exit criteria:

- Truncation, checksum, incompatible identity, out-of-space, OOM, cancellation,
  and injected subsystem failure leave the live game or previous slot usable.
- No Java UI thread blocks on serialization.

## Phase 6: Halo validation

- Save at the known campaign point, advance, and restore repeatedly in one
  process.
- Compare CPU/memory/kernel identities and EDRAM/first-frame hashes.
- Verify audio recovery, input release, filesystem generation, and renderer
  stability across repeated cycles.
- Treat reproduction of timing-sensitive corruption as an observation, not a
  guaranteed property of semantic save-state restoration.
