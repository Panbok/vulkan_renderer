---
status: proposed
updated: 2026-10-03
authority: proposal
---
# Metal follow-ups from the Windows Vulkan sessions

The Windows host (RX 6700 XT, Vulkan) changed shared and Vulkan code that
the Mac has not built or run. This list gives the Metal work, the check that
closes each item, and the Windows evidence to compare against. Do the items
in order: A and B are builds of code that is already on `main`.

## A. Build the script commits on macOS

Sixteen script commits (`e9be97ef` to `71ecc1cd`) were written and tested only
on Windows: the `sdk.h` ABI with ledger lifetimes (`2db591f3`), structural
edits queued in fixed updates, script tasks on workers, models spawned on
resource workers, project script packages, the Jolt job adapter (`9b0d5455`),
the job-system wake change (`249a9469`), the clock-free `vkr_rand_i32`
(`5b9c2c14`), packaged project script libraries (`38b49d16`) and component
migration (`71ecc1cd`).

1. Run `./build_editor.sh Release` and `./build_test.sh`. All suites must pass.
2. Open a project with `Scripts/` in the editor. Check that the first build
   runs on a worker and that a reload migrates changed component fields.
3. Package a game with project scripts. The script library must sit in the
   app's `Frameworks` and load before the World (`38b49d16`; the macOS run
   is not done).

## B. Compile the shared light-contribution flag on Metal

`750db2bc` measures light contribution only in frames whose index is a
multiple of `VKR_LOCAL_LIGHT_CONTRIBUTION_PERIOD` (4). Metal now reads the
shared `prepared_frame.light_contribution_enabled` in
`vkr_metal_packet_plan_frame_submission`
([`vkr_metal_packet_frame.inc`](../../renderer/src/metal/internal/vkr_metal_packet_frame.inc)).
This line was not compiled.

- Build Metal Release and run `local_shadow_cache_bistro_metal_indoor_walk`.
- Check that `lighting.local_shadow.*` metrics match a run before `750db2bc`
  and that mean `Lighting.Deferred` time falls. On Vulkan at 1920x1080 with
  TAA it fell from 3.68 to 3.54 ms.

## C. Check Metal frame pacing under TAA

On Vulkan, TAA frames alternated 10, 20 and 2 ms (p95 20.5 ms against an
11 ms mean). The exposure resolve read the newest *completed* exposure state
on the GPU and kept that old history instance in use until the frame
completed. The shared history ring then had no free index every third frame
and waited for an in-flight frame. `ba889215` passes that state from the
completed frame's readback instead (ADR-042).

Metal selects the same record
(`vkr_metal_packet_select_exposure_history` in
[`vkr_metal_packet_graph.inc`](../../renderer/src/metal/internal/vkr_metal_packet_graph.inc))
with two frame slots and a four-instance ring.

1. Run `local_shadow_taps_bistro_metal_street_taa` under
   `local-offscreen-perf-audit-gpu` in Release with validation unset.
2. Compare `frame.wall` p50 with p95. Read per-frame values from the run's
   `samples.bin`: after the 496-byte header come `metric_count` names of
   152 bytes, aligned to 8, then float64 values in frame-major order
   (`frame * metric_count + metric`).
3. If the history-output selection waits periodically, port the fix: put the
   completed exposure state from the frame's readback
   (`readback_layout.exposure`) into the frame upload and point
   `previous_state` at it, so no GPU read keeps the old instance.

## D. Measure two fully filtered local lights on Metal

`f74de4c4` lowered `VKR_LOCAL_SHADOW_FULL_FILTER_LIGHT_COUNT` from 3 to 2 in
shared host code, so it also applies to Metal. On Vulkan at 1920x1080 with
TAA, `Shadow.LocalMask` fell from 2.84 to 2.66 ms and 0.14% of pixels changed
by more than 8 of 255 (ADR-019). Metal has no measurement.

- Run the Metal street and indoor TAA capture and timing cases before and after
  `f74de4c4`. Record mask time, frame time and the capture difference in
  ADR-019. If the change on Metal is visible, raise it with the owner.

## E. Size Metal texture layer tables by the render slots

`VKR_METAL_PACKET_MAX_TEXTURE_LAYERS` equals `VKR_LOCAL_SHADOW_FACE_COUNT_MAX`,
which `c87bce97` raised from 64 to 768. It sizes per-texture layer view arrays
([`vkr_metal_packet_renderer.m`](../../renderer/src/metal/vkr_metal_packet_renderer.m)).
On Vulkan the same coupling made renderer creation fail, which `f0e2a36c`
fixed. The largest layered graph image holds
`VKR_LOCAL_SHADOW_RENDER_SLOT_COUNT_MAX` (64) layers.

- Set the Metal limit to the render-slot count, add the static assertion that
  the atlas and mask layer counts fit, and check renderer memory and startup.

## F. Capture one transmission layer on Metal

`9e356b7e` fixed Vulkan captures of one layer or mip, which used the terminal
layout of subresource 0. Run `tools/cases/smoke/transmission_bistro_exterior_door.case.json`
on Metal and confirm that all six channels capture, including
`transmission_visibility_ids_layer_4`.

## G. Close open Metal gates in ADR-044

- The picking depth readback text says the Metal side was not compiled;
  `04bb79bb` later fixed the Metal root. Run a Metal pick and grid fit, then
  update the text.
- The near-camera editor grid change was not compiled for Metal. Check the
  grid at 0.25 and 1.7 m on Metal.
- Editor surface snapping and version 5 scene saves (`6077b057`, `52d5d5f2`)
  were checked only on Windows.
- Every domain remains UNALIGNED until matched Metal and Vulkan captures pass.
  Publishing a Metal baseline generation of the local-shadow capture cases
  lets Windows run `snapshot --cross-backend`.

## Deferred cross-backend work

These change shared formats or the graph. Measure the Metal cost first and
decide with the owner before starting:

- Compact transmission layers 1 to 3 from the layer-0 pixel list instead of
  full-screen scans; about 0.2 ms on Vulkan at 1080p.
- Store untinted `Shadow.LocalMask` slots in fewer channels; mask reads cost
  0.73 ms of Vulkan deferred lighting.
- Pack the local-shadow transmission depths into one texture; transmission
  lookups cost 0.83 ms of the Vulkan mask, because every Bistro lamp is inside
  its own lantern glass.

The Vulkan cost split, negative results and method are in
[lighting-efficiency.md](lighting-efficiency.md#vulkan-cost-split-2026-10-03).
Vulkan pass intervals now start no earlier than the previous pass's end for
compute and graphics (ADR-051); Metal applies that to graphics passes only.
