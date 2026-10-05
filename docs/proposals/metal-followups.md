---
status: proposed
updated: 2026-10-05
authority: proposal
---
# Metal follow-ups from the Windows Vulkan sessions

The Windows host (RX 6700 XT, Vulkan) changed shared and Vulkan code that
the Mac had not built or run. On 2026-10-03 the Mac (Apple M1 Pro) built and
checked that work. The results are in their owning documents:

| Work | Result | Owner |
|---|---|---|
| Script commits, editor first build, packaged script library | Pass after the Jolt adapter RTTI link fix | [ADR-079](../adr/079-c-script-modules.md#evidence) |
| Light contribution every fourth frame | Pass; Metal deferred time within noise | [ADR-019](../adr/019-bounded-forward-spatial-lighting.md) |
| TAA pacing from exposure history | No periodic wait on Metal; no port | [ADR-042](../adr/042-scene-linear-post-processing.md) |
| Two fully filtered local lights | Mask 0.29 ms faster in the street view; no visible change | [ADR-019](../adr/019-bounded-forward-spatial-lighting.md) |
| Metal texture layer tables | Sized by the render slots; 186 MB less resident memory | [`vkr_metal_packet_renderer.m`](../../renderer/src/metal/vkr_metal_packet_renderer.m) |
| One-layer transmission captures | All six channels of `transmission_bistro_exterior_door` capture | This section |
| Picking depth, near-camera grid, snapping, version 5 saves | Pass on Metal | [ADR-044](../adr/044-shader-cross-backend-contract.md), [ADR-076](../adr/076-project-object-model.md) |

The `transmission_bistro_exterior_door` snapshot (Metal Release,
`local-offscreen`, report digest `sha256:410cd1b6…`) captured
`transmission_visibility_ids_layer_4` with 191 non-zero identifiers against
63,398 in layer 0.

## Remaining: cross-backend captures

Every ADR-044 domain stays UNALIGNED until matched Metal and Vulkan captures
pass. Two backend-neutral Bistro cases capture local shadows without TAA, in
the street view and at the indoor owner camera:
[`local_shadow_bistro_street_capture`](../../tools/cases/local/local_shadow_bistro_street_capture.case.json)
and
[`local_shadow_bistro_indoor_capture`](../../tools/cases/local/local_shadow_bistro_indoor_capture.case.json).
They use the Bistro snapshot comparison policy: at most 0.002% of pixels may
differ, by any amount, with a mean error of at most 0.0005. Their first Metal
generations (`local.offscreen`, M1 Pro, 2026-10-03) are published under
[`tools/baselines/local.offscreen`](../../tools/baselines/local.offscreen); a
repeat Metal snapshot of each passed with no failing pixel.

The first Windows run could not compare them: the workload fingerprint hashed
the cooked BC and ASTC textures, which differ by design. The scene content
digest is now host-neutral ([ADR-051](../adr/051-renderer-harness-and-evidence.md)),
which changed every workload fingerprint once. On the Mac:

1. Build at the revision that carries the change and recook Bistro's meshes;
   the glTF source fingerprint now ignores CR line endings, and LF sources keep
   their fingerprints.
2. Snapshot and re-accept `smoke.sh_ibl.single_probe.snapshot`. The other
   three Metal generations were re-accepted on 2026-10-05 together with
   Bistro's lamp source radii (ADR-019): street `e9391d67…`, indoor
   `4d9fdfd3…` and `smoke.bistro.metal.text.snapshot` `ac640a40…`; a fresh
   snapshot of each passes against its generation.
3. On Vulkan, three runs of the text snapshot with identical inputs differ in
   captures 5, 8, 9, 12 and 13, by up to 6.3% of pixels (peak 157/255), on
   foliage and pot shadows near the lamps. The local-light contribution
   readback, which picks the fully filtered lights, is the suspected source.
   On Metal, two runs at `20eb4355` differ in at most 11 pixels per checked
   view, and none in view 3.

Windows then runs each local-shadow case with `vkr_harness snapshot --profile
tools/profiles/local-offscreen.json --cross-backend` and records the result in
ADR-044.

## Deferred cross-backend work

These change shared formats or the graph. Decide with the owner before
starting:

- Compact transmission layers 1 to 3 from the layer-0 pixel list instead of
  full-screen scans. The scans cost about 0.2 ms on Vulkan at 1080p and
  0.50 ms on Metal at 1280x720 with TAA (`Transmission.Compact.Fullscreen.1`
  to `.3` in `local_shadow_taps_bistro_metal_street_taa`).
- Store untinted `Shadow.LocalMask` slots in fewer channels; mask reads cost
  0.73 ms of Vulkan deferred lighting. Not measured on Metal.
- Pack the local-shadow transmission depths into one texture; transmission
  lookups cost 0.83 ms of the Vulkan mask, because every Bistro lamp is inside
  its own lantern glass. Not measured on Metal.

The Vulkan cost split, negative results and method are in
[lighting-efficiency.md](lighting-efficiency.md#vulkan-cost-split-2026-10-03).
Vulkan pass intervals start no earlier than the previous pass's end for
compute and graphics (ADR-051); Metal applies that to graphics passes only.
In the Metal street TAA view the pass sum is 13.70 ms against a 14.34 ms
frame, so Metal compute rows do not visibly overlap.
