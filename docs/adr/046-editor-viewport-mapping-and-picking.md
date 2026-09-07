---
status: implemented
updated: 2026-09-07
authority: adr
---
# ADR-046: One editor viewport mapping for scene presentation and interaction

## Status

Accepted.

## Context

A docked editor Scene panel has an output rectangle, an optional internal Scene
extent, and pointer coordinates. Camera projection, composition, picking, and
gizmos must agree on the same mapping.

## Decision

`VkrViewportMapping` is the source for conversion between the dock-owned Scene
panel, the scene target, and normalized coordinates. The editor derives this
mapping from the current dock panel rectangle and uses it to build the packet
viewport payload. The renderer realizes Scene resources at the mapped extent,
composites Scene output into the panel rectangle, and draws editor UI afterward
at the native drawable extent.

Picking requests use mapped scene coordinates. Picking IDs distinguish scene
entities and gizmo handles. Scene-only mode uses the complete drawable mapping
while preserving the dock tree.

The application borrows up to nine `VkrEditorOverlayDraw` records through packet
submission. The gizmo system owns their published geometry references; native
frame upload storage owns prepared roots until GPU completion. Handles use a
world-axis model scaled to 150 displayed pixels per unit, independent of camera
distance and internal render scale. Both native pipelines use the unjittered
camera projection. Color is opaque, unlit linear RGB, drawn after tonemapping
into the retained Scene image. It does not enter lighting, shadows or temporal
history.

The overlay picking pass loads the scene picking image after feature picking and
writes encoded handle IDs before readback. Both overlay passes disable depth
testing and culling and consume the same ordered handles: hovered and active
handles draw last. Visible handles therefore take priority over scene surfaces
and overlapping handles use the same order for color and picking. This requires
no extra scene-sized image. Translation arrows, rotation rings and scale cubes
are shown together; all scale cubes perform uniform scaling.

Edits preserve the existing local TRS ownership and undo transactions. Translation
uses the inverse parent transform; rotation converts through parent orientation
while preserving local scale. Rotation under a nonuniformly scaled parent does
not introduce shear and cannot represent every exact affine world rotation.
Inspector remains available for numeric editing. Singular or non-TRS transforms
that cannot support the drag are excluded at the interaction boundary.

Simulation and Scene rendering have independent runtime states. The editor starts
with simulation paused; pausing passes zero scene delta while allowing dirty scene
synchronization. The scene update currently has no animation or physics simulation.

`VkrEditorPassPayload.scene_rendering_stopped` suspends Scene extraction and graph
passes while keeping UI submission active. Running editor frames resolve post-tonemap
Scene color into a renderer-owned retained image before compositing it. Stopped
frames sample the last successfully submitted image at its stored resolution, fitted
into the current panel. They clear the viewport if no image has been submitted.
The image uses one physical instance, native queue ordering, and graph last-use
retirement. Stopped panel/window resizing does not resize that image. Resume
refreshes its extent and invalidates temporal continuity. UI-only submissions do
not publish Scene temporal or exposure history. Picking and camera manipulation
are disabled while stopped. Hiding the Scene tab also suspends Scene work.

Vulkan resets each frame slot's prepared Scene readback list before accepting a
UI-only frame. Such frames have no draw-compaction, shadow-reduction or exposure
producers. The readback recorder still clears old statistics, but copies no
Scene buffers; live frames retain their required-producer checks. Slot reuse
continues to require completion of its last submission.

The user approved the extra scene-sized image and compositing pass so Render Stop
preserves the visible scene while the editor and simulation remain independent.

A Scene out-of-memory failure reduces Application-owned output scale by 0.75 and
retries on a later frame, down to a 0.25 floor. Metal's existing dynamic-resolution
ratio and MetalFX operate within this reduced Scene output; other paths multiply
their existing viewport scale. The UI stays at window resolution. The reduced
scale persists across Stop/Resume and asynchronous Scene activation; unloading
resets it. The viewport reports the last successfully submitted render/output
dimensions and the current fallback percentage without covering toolbar controls.
If the minimum still fails, or another recoverable submission error occurs,
Render Stop latches a visible error until explicit retry or unload. UI-only frames
continue instead of retrying an impossible allocation every frame. Both native
backends preserve graph-allocation OOM status for this policy.

The user approved bounded texture recovery in the paneled editor and its
extension to the standalone Metal app. Only typed
out-of-memory publication failures retain the existing material/slot/path record;
the failed resource request and prepared payload are released. A successful live
Scene submission commits its output-reduction generation and allows one retry
per waiting texture. Each attempt records that generation, so late failure
notifications can use relief that occurred after the attempt began. Another OOM
waits for another reduction. Waiting textures remain pending in readiness metrics;
invalid files and other publication failures remain terminal. Texture memory
pressure can request the same bounded output reduction even when the current
Scene graph still renders. Demanded budget evictions also count as missing and
pending; they request the same relief and retry once per committed reduction or
actual residency-budget increase. The viewport shows waiting and demanded-missing
counts while incomplete. After committed Scene relief, the asset owner refreshes
the automatic texture allowance from current memory usage. Metal accounts asset
heap capacity separately and permits reuse of already charged capacity. Its
80% target, 90% pressure-entry and 75% exit thresholds remain unchanged; finite
capacity retries require a new high-water allowance as described in
[ADR-024](024-shared-bindless-gpu-cores.md). Further texture-triggered reductions wait until
in-flight requests finish; a native frame OOM still requests immediate relief.
Unload removes waiting records with their materials.

Standalone Metal applies this scale to its full physical target extent. Its
fullscreen tonemap stretches the reduced Scene output to the drawable, then UI
renders at native resolution. Both MetalFX and fixed-scale spatial modes honor
the override; temporal preparation and picking use the resulting render extent.
Application owns the output scale and relief generation for both presentations,
and configures material recovery before asset publication. The runtime no longer
restricts retry admission to the editor. If the floor cannot relieve an app OOM,
the app exits through normal shutdown rather than repeating failed allocations;
the paneled editor keeps its existing Render Stop behavior. Standalone Vulkan
has no Scene-output scaling capability and retains its existing unscaled texture
failure behavior. GPU completion and the 4 GiB default managed cap are unchanged.

On a live Scene image-allocation OOM, Metal retries image realization once at
the same requested extent. Before that retry it waits for all submitted work,
retires mismatched owned images and non-grow-only buffers, and collects completed
storage, including detached old generations from partial realization. Unchanged
images remain retained. If the retry also fails, the existing bounded output
reduction applies. Ordinary successful dynamic-resolution changes keep their
replacement policy; this completion wait occurs only during allocation recovery.
Reclaiming a mismatched retained Scene image invalidates its contents; stopped
frames use Clear if no valid content survives. A stopped frame does not enter
this live-Scene retry path.
Scene-image budget and allocation attempts retain their low-level details in
opt-in diagnostics. A successful retry emits one warning with the image and
reason; a terminal failure emits the detailed allocation error. Asset textures,
buffers and external allocation failures keep their immediate budget errors.
With the editor logging configuration, a Bistro shrink/resume round trip under
a test-only 3800 MiB cap recovers two allocations, emits exactly two warnings and
zero errors, and retains all 517 texture assignments. A tiny API-validated forced
failure keeps its terminal allocation error. The normal app/harness Release
configuration compiles out warnings; the editor Release configuration includes
them. That run's report SHA-256 is
`89f3d1984f99f8e971d4cb749f89e035e51c28bbd786a235c146c0a5ad31d362`.
Stopped presentation queries the native retained
image's current generation, valid submitted content and extent; a failed resize
cannot reuse stale frontend dimensions to sample an uninitialized replacement.
Without valid retained content the graph clears the Scene panel and does not
allocate a placeholder retained image. The managed memory cap remains unchanged.
Reducing Scene dimensions cannot resolve every texture, geometry or command-buffer
allocation failure; the bounded retry policy must terminate at the floor.

The user approved process termination after the first five-second Metal completion
timeout. The wait owner flushes enabled diagnostic segments, writes the target,
completed and submitted serials to stderr, and calls `_Exit(EXIT_FAILURE)`.
It bypasses normal shutdown and exit handlers because resource completion has
not been proven. Unsaved edits are lost. This bounds application retries after
a timeout; it does not prevent a GPU or operating-system hang.

## Consequences

No caller may independently infer camera aspect, picking coordinates, gizmo
coordinates, or compositor placement from window size. A panel resize changes
the mapping before the next packet. Picking remains asynchronous and may be
pending when its completion has not arrived.
Each interaction request carries a monotonically increasing identity echoed by
native readback. Completed pixels are copied before their GPU storage is reused;
cancelled selections ignore older results without waiting for the GPU.
Drag requests retain the press position and the first release position in
normalized displayed-image coordinates. Internal render-size changes do not
cancel pending picks or active edits; GPU request pixels are remapped when the
packet is prepared. A gesture that ends before asynchronous picking completes
still applies its final transform as one undo transaction. A handle click with no
displacement preserves history. Recoverable Scene errors cancel invalidated GPU
picks while retaining active edits; stopping Scene rendering commits the current
edit. Escape explicitly restores the pre-drag transform.

## Verification and limits

The standalone-app extension passes normal Release Bistro checks on Apple M1 Pro
with a 3024×1898 physical target and the unchanged 4 GiB cap. The MetalFX case
loads all 517 texture assignments with zero pending, failed, demanded-missing,
or evicted assignments after reducing Scene output to 75%; dynamic resolution
then renders at 1134×712. The fixed spatial case renders at 1701×1068 after bounded
relief, captures picking IDs at that extent, and presents final color at
3024×1898. Its report SHA-256 is
`c70b83a05b6ff4f4424547c759ce42e19591c15be351a2f267e371d0f2521ab4`.

Reproduction commands use `./build_release/tools/vkr_harness snapshot --case`
with `tools/cases/local/app_bistro_texture_memory.case.json` or
`tools/cases/local/app_bistro_texture_memory_spatial.case.json`, followed by
`--profile tools/profiles/local-metal-windowed-validation-serial.json`.
Validation variables are unset; the profile name does not enable native validation.
The existing `editor_memory_same_resolution_retry` snapshot passes Stop, resize,
Resume and both 1528×1074 extent assertions (report SHA-256
`d19ebc4b3cef6d04963b8404e64a31868d29621355ac37d5360b7acb60350377`).
These observations establish bounded startup recovery and composition, not a
performance result, long-session memory stability, or native Vulkan acceptance.

The Vulkan stopped-frame readback fix passes Release and Debug checks on an
RX 6700 XT at 100% Windows scaling. The
[`editor_vulkan_empty_stopped`](../../tools/cases/local/editor_vulkan_empty_stopped.case.json)
case submits six frames without any Scene image. The
[`editor_vulkan_stop_resize_resume`](../../tools/cases/local/editor_vulkan_stop_resize_resume.case.json)
case stops at frame 1, reuses slots while stopped, resizes 320×240 to 400×300
and back, then resumes at frame 6. Both use
[`local-windowed-single`](../../tools/profiles/local-windowed-single.json) with
`vkr_harness profile --case <case> --profile <profile>`.
The transition case also passes `vkr_harness snapshot` in Release and under
Debug Vulkan synchronization validation, with no validation errors. Its small
fixture captures show [stopped](../../assets/editor/vulkan-render-stopped-fixture.png)
and [resumed](../../assets/editor/vulkan-render-resumed-fixture.png) presentation;
they do not establish scene-quality or cross-backend pixel parity. Release
snapshot report SHA-256:
`4573e88ac04804604a406ab86ac11b59f15c027ab22e272acd763f62be8406e1`.
That check also exposed and verified the mapped-buffer alignment correction in
[ADR-024](024-shared-bindless-gpu-cores.md). No shaders or shader ABI changed.

A focused CPU interaction check reproduces the old extent-change rollback and
passes with normalized gesture coordinates, including translation, rotation,
uniform scale, delayed release, and undo/redo. A normal Release Bistro editor check also passes native
Metal translation and undo/redo with 1528×1074 Scene output. Native rotation and
scale across resolution changes, Vulkan execution, and forced allocation-failure
recovery remain unverified for this change.

The transport and bounded-retry checks pass on Metal after the autorelease and recovery changes.
Normal Release full-size Bistro exercises 5,985 nodes, toolbar unload/reload,
Stop/resize/Resume and Scene-focused Tab. A second load encounters texture and
Scene allocation pressure, recovers to 645×453 output, and ends with zero pending
requests and zero terminal texture failures across 1,034 texture assignments.
Smaller forced-budget runs verify automatic reduction and a readable stopped
error at the floor. Both Release wrappers and a final six-frame Metal API
Stop/resize/Resume check pass; the latter's report SHA-256 is
`40a04fe6d1615e69ad1dda39d1fcc99a5059492a60892b49c99a3a2e1d4e10b6`.
These bounded checks do not establish long-session stability, native Vulkan
behavior or the cause of the earlier panics below.

A subsequent visible-texture review found that historical assignment and failure
counts do not prove current residency. Current recovery now counts demanded
missing and evicted assignments, retries after capacity relief, and ranks shared
textures by all users' demand. Published asynchronous textures retain a request
reference through GPU completion. CPU ownership checks and both Release builds
pass. The subsequent automatic-budget feedback loop is corrected by charged
texture-capacity accounting and finite high-water retries. A bounded 140-second
normal Release Bistro run exercises two loads under the 4 GiB managed cap,
ending with 517 resident assignments and zero missing, pending or failed textures.
Both unloads restore the same live texture count and bytes. The initial load is
followed by 109 seconds without texture-load events before explicit reload.
The earlier capacity-recovery screenshots show textured surfaces after both loads. Scene output falls to 31.6%
on the first load and 56.25% on the second; this does not prove full-resolution
acceptance, pixel equivalence or long-session stability. The two-load run ended
with 517 resident assignments and zero missing, evicted, pending, in-flight, or
failed assignments; it observed 2,552,201,888 bytes of unique streamed texture
payload, 2,684,354,560 bytes of dedicated texture-heap capacity, and
3,857,682,480 bytes of managed allocation under the 4 GiB cap. The compressed
asset set, transfer storage, and retained heap capacity are separate quantities.

Demand-sized graph draw tables, post-publication budget sampling and one
completion-gated image-allocation retry now avoid that memory-resolution fallback
in the tested 1528×1074 editor configuration. A bounded 115-second normal Release
run completes two Bistro loads with 517 resident assignments, zero missing,
pending, failed or evicted textures, and six successful same-resolution allocation
recoveries. Scene output stays 1528×1074; MetalFX still varies internal resolution
for its existing frame-rate target. Stop retains the visible image, and unload
returns to 10 live texture allocations / 12.368 MiB.
A separate full-target spatial run disables docking and dynamic resolution to
verify actual 1528×1074 internal rendering, all texture assignments resident and
zero draw overflow under the same 4 GiB cap. This distinction matters: the nominal
renderer size metric alone does not prove the packet viewport size in spatial
paneled mode. A focused analytic resize case passes Metal API validation;
smaller diagnostic caps exercise the single retry and bounded failure without
validation errors. These are bounded memory/lifetime checks, not long-session or
native Vulkan proof. The full-target fixed-scale Bistro case exercised 1528×1074
rendering with 517 resident assignments and no missing, failed, or evicted
assignments; it exited 0 with report SHA-256
`8cc36e5e8310b185bf84ac7c983f7329e14af50bcdd74cad0762726f56ecfd9a`.

A subsequent normal Release tiny-scene UI run passed Console filtering, Scene
focus, toolbar snapping and unload/reload, then stalled after Render Stop with
completed/submitted serials 11387/11389. Its stack was waiting for a Metal command
slot. No allocation failure was logged and managed memory was about 1.83 GiB.
This remains separate from Bistro's allocation exhaustion; the root cause is
unresolved. A subsequent tiny-scene run with a test-only 1408 MiB cap displayed
the allocation-error banner, then produced a kernel data abort in IOGPUFamily
and AGXG13X. The last diagnostic began waiting for serial 3619, with completed
3618 / submitted 3620; no wait return or timeout-exit marker was recorded. The
stackshot attributed 13.37 GiB resident memory to the editor, far beyond the
managed cap. A missing autorelease-pool boundary around Metal rendering was
then identified in source; its contribution to the panic remains unproven.
The first-timeout exit has not been exercised to completion by an actual GPU
timeout. The preserved watchdog investigation remains inconclusive; no panic
cause is asserted here.

The first-stopped and Bistro frozen-resize/resume cases pass serial Metal API
validation. A five-frame Release Bistro orbit captures byte-identical last-live
and stopped images; the resumed capture changes with the camera. A bounded
MetalFX/dynamic-resolution resize/resume case also passes. Native Vulkan execution
and bilateral image comparison remain unavailable on this host.

Two interactive runs produced the same macOS GPU firmware data abort
(`agx_background`, 2026-09-06). The second used Debug with Metal API and shader
validation disabled, after stopping Scene rendering and selecting a hierarchy
node. Its matching executable symbols place the editor in a command-slot
completion wait. This identifies the CPU wait, not the triggering GPU command;
the cause remains unconfirmed. These failures preceded the final recovery checks.
The bounded checks above do not establish editor stability.

Subsequent offline review corrected missing layer residency registration and
publication-failure collection that used submitted rather than completed serials.
A single Release empty-scene diagnostic now passes stopped-compositor startup,
completion and teardown with layer residency registered. It does not exercise
publication failure or reproduce the Bistro interaction; the panic cause remains
open. A subsequent tiny-node Release case also passes stop/resize/resume:
logged retained extent stays fixed while stopped, and all submissions complete
before teardown. Neither diagnostic captures pixels. The empty stopped case
completed 96/96 submissions (report SHA-256
`ff5c2ab91b67b58a98922117b8193ef0ac0dfc99ab726e16ee76006846f83cb6`); the
node transport case completed 99/99 after 640×480 → 800×600 → 640×480 resize
(report SHA-256
`ea0b3644fd46d73cd11b75b6c90da406f8165a81f592bf5fae0a3c865a1d9fc7`).
The drawable release-after-present pattern itself matches Apple's sample.

A subsequent serial Bistro run with Metal API validation enabled (shader
validation disabled) produced a system watchdog timeout on 2026-09-06. It stopped
during startup before scene readiness or the authored transport actions. The
last diagnostic record begins waiting for serial 29, with completed 28, before
the logging flush and native wait; it cannot distinguish those blocking sites.
The stackshot records 15.34 GiB wired memory on a 16 GiB host and 10.18 GiB resident
memory for the harness. The then-fixed 7 GiB resident placement heap exposed a memory-budget defect;
neither memory exhaustion nor validation is established as the panic cause.

The approved [demand-created heap and 4 GiB managed cap](024-shared-bindless-gpu-cores.md)
now replaces that allocation policy. Empty startup and tiny-node transport pass,
including one serial Metal API validation case after the memory changes. A
normal Release Bistro case initially reported texture-budget failure promptly.
Demand-grown ICBs subsequently let that bounded 320×240, one-cascade case reach
complete texture readiness and pass transport under the unchanged cap. Larger
interactive configurations can still exhaust the budget. The bounded empty
default-cap case completed 96/96 submissions with 3,239,776,304 managed bytes
(report SHA-256 `4a32471743f08d500c0fe82928de64c2224383c4c9e952f575bdba0f7117affd`).
The 1024 MiB case rejected startup as expected; the tiny-node API case completed
99/99 without API errors. These cases are separate from Bistro acceptance.
The later full-size interaction checks above extend this evidence; the panic investigation remains open.

After geometry range reuse and material/shape ownership corrections, a serial
Metal API validation run alternates the small node fixture with three added cubes
and back twice. Each unload returns to the same live default ranges, completed
ranges are reused without backing growth, and teardown releases every range
without validation errors or resource warnings. A final shadow/transmission
capture is byte-identical to its earlier witness; the bounded Bistro case passes
again. The final capture report SHA-256 is
`c550cbcd50a19730a607c53c9281d9e69182dd824fa9e58df69b058a68d96703` and the
bounded Bistro report SHA-256 is
`1ac6de36f7edf3d873cd97bee82ceca40c19d58188d75e256aa7cdfb1948dd06`.
Those range checks alone do not
establish the cause of the earlier panics.

Visible handles now pass native Metal interaction checks on the small node
fixture: translation, rotation, uniform scale, undo/redo, independent child
movement, and no-displacement clicks preserving redo history. A serial API
validation run passes translation, rotation, scale and undo without reported API
errors; a separate normal Release run supplies screenshots and the remaining
interaction checks. The projection oracle checks 150 displayed pixels across
camera distances, drawable extents and internal scales. Production Slang entries
compile, but native Vulkan execution and bilateral pixel comparison remain
unavailable. Mid-drag Escape and nonuniform-parent rotation were reviewed in
source but were not exercised by the native UI checks. The final native Metal
API editor run exited 0 without reported API errors and completed 28,070/28,070
submissions; the normal Release interaction run also exited 0 after verifying
translation, rotation, uniform-scale undo, and redo.

## Alternatives considered

Separate mappings for camera, picking, and composition drift when a dock split,
fit mode, or internal Scene scale changes. A retained editor mesh duplicates
the panel rectangle without owning the dock decision.

## Revisit when

The editor supports multiple simultaneous Scene panels or a presentation mode
that cannot be expressed by the existing panel-to-target mapping.

## Code evidence

- [viewport mapping](../../lib/src/renderer/systems/vkr_editor_viewport.c)
- [picking lifecycle](../../lib/src/renderer/systems/vkr_picking_system.c)
- [gizmo IDs](../../lib/src/renderer/systems/vkr_gizmo_system.h)
- [editor interaction caller](../../runtime/src/vkr_sample_runtime.c)
