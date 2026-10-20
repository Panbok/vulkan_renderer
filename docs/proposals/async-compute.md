---
status: proposed
updated: 2026-10-20
authority: proposal
---
# Asynchronous compute

## Current baseline

- The Vulkan device selects the first queue family that supports graphics,
  compute and transfer, plus present for a window, and creates one queue
  from it (`vkr_vk_select_candidate_queue` in
  [vkr_vulkan_device.c](../../renderer/src/vulkan/vkr_vulkan_device.c)).
  It does not look for a compute-only family.
- Each frame slot records one command buffer. `vkr_vk_queue_submit_frame`
  ([vkr_vulkan_renderer.c](../../renderer/src/vulkan/vkr_vulkan_renderer.c))
  submits it once and signals the renderer's single timeline semaphore.
  Resource reuse compares `last_use_submit_value` records with that timeline.
- The graph has graphics, compute and transfer pass types (`VkrRgPassType` in
  [vkr_render_graph.h](../../renderer/src/vkr_render_graph.h)). Compute and
  transfer passes use the graphics submission path, and the compiler emits
  no ownership transfers
  ([Graph and native execution](../ARCHITECTURE.md#graph-and-native-execution)).
  Barriers use `VK_QUEUE_FAMILY_IGNORED`, buffer barriers cover whole
  buffers, and transient allocations do not alias.
- Some work is outside the graph's declarations. The TLAS is not a graph
  resource; `Volume.Build` and its readers are ordered through the declared
  `ray_scene` buffer ([ADR-119](../adr/119-dynamic-diffuse-volume-layer.md)).
  `Volume.Trace` declares no writes, and `IBL.Bake` keeps its accesses
  backend-owned.
- Metal records each run of consecutive compute and transfer passes in one
  compute encoder on the frame's `MTL4CommandQueue`. The editor presenter
  composites on a second queue ([ADR-111](../adr/111-editor-presenter.md));
  no graph pass runs there.
- Uploads are out of scope; [Dedicated transfer queue](dedicated-transfer-queue.md)
  owns them. The baseline frame cost comes from
  [Desktop frame budget](desktop-frame-budget.md).

## Reference: idTech

- Doom (2016) used async compute on AMD GPUs under Vulkan, only in its
  TSSAA 8tx mode.
- DOOM: The Dark Ages (idTech 8) runs triangle attribute interpolation beside
  the pixel-command build, the world light grid and the radiance cache
  updates on async compute. In the GI work alone, async compute saved about
  0.5 ms on Xbox Series X and PS5 and about 0.4 ms on Series S (Sousa,
  SIGGRAPH 2025).
- id also tried deferred texturing on async compute and found no place for
  it: after the move to a visibility buffer, too little low-ALU graphics
  work was left to overlap (Lazarek and Hammer, GPC 2025).

Async compute pays only beside graphics work that leaves ALUs idle. In a
visibility-buffer frame, that work is the depth-only and ID-only raster.

## Overlap candidates

Each row is a hypothesis until a GPU trace shows idle ALUs during the
graphics work and a matched report shows a frame-time gain.

| Async compute work | Graphics work to overlap | Why the dependency allows it |
|---|---|---|
| `Sky.ViewLut`, `Sky.AerialPerspective`, `Clouds.Shadow` | `Shadow.Cascade.*`, `Shadow.Local.*` | They read no graph resource that this frame writes. |
| `Volume.Build`, `Volume.Trace` (1.0 ms budget) | Shadow raster, `VBuffer.Opaque` | They read skinning output, candidate rows and `ray_scene`. The trace's output must first become a declared write. |
| `Froxel.Inject`, `Froxel.Integrate` | `VBuffer.Opaque`, `VBuffer.Transmission.*` | The injection reads only the shadow maps and `cloud_shadow`. |
| `Shadow.Moments.*` | `Shadow.Local.*` | It reads only `shadow_map`. |
| `AO.PrefilterDepth*`, `Clouds.Trace`, `HZB.Build*` | `VBuffer.Transmission.*`, `Reflection.Raster` | They read `opaque_vbuffer_depth` and, for clouds, `aerial_perspective`. |

## Proposed work

1. **Queue selection.** Look for a compute-capable family without graphics
   and create one queue from it. Without one, async passes record inline in
   graph order, with no behavior change. The device report names both
   families.
2. **Authored opt-in.** A per-pass field in the graph JSON, for example
   `"queue": "async_compute"`, valid only on compute passes. Compilation
   fails when such a pass touches a resource that the graph does not declare.
3. **Scheduling.** The compiler splits the frame into per-queue segments at
   cross-queue edges. Each edge becomes a timeline wait in the consumer's
   submission; barriers inside a queue stay as today.
4. **Ownership.** Keep `VK_SHARING_MODE_EXCLUSIVE` and emit release and
   acquire barriers at cross-queue edges. Skip the pair when the consumer
   discards the previous contents.
5. **Completion.** The compute queue signals its own timeline semaphore. The
   frame's last graphics submission waits for the compute queue's final
   value, so the frame `submit_value` still proves completion of all of the
   frame's work, and retirement code keeps its single value.
6. **Timing.** The GPU pass timeline
   ([ADR-126](../adr/126-cpu-zones-and-gpu-pass-timeline.md)) labels each
   pass's queue, so a trace shows overlap.
7. **Metal.** No change here. The tiled class evaluates concurrent compute on
   its own evidence under [ADR-087](../adr/087-gpu-class-graphics-pipelines.md).

## Owner decisions (2026-10-20)

The owner accepted each recommendation below on 2026-10-20.

- **Opt-in or automatic assignment.** Decision: authored opt-in per
  pass. Automatic assignment hides the cost model and makes a regression
  hard to bisect.
- **Exclusive ownership or concurrent sharing.** Decision: exclusive
  with transfers. Concurrent sharing can disable image compression on some
  GPUs; measure it only for small buffers.
- **Frame-end join or per-queue retirement.** Decision: the frame-end
  join. Per-queue last-use values would change every retirement site.
- **Submissions per frame.** Decision: at most three at first
  (graphics, compute, graphics); each submission has CPU cost.
- **Control.** Decision: a developer switch that forces inline
  execution for A/B runs and diagnosis, not a user setting.

## Evidence needed

- The desktop frame budget and its Bistro baseline.
- Matched capture-free Release reports on Bistro, before and after, with
  valid samples and spread, on a device with a compute-only family. Confirm
  the family on the Windows host with `vulkaninfo` first.
- A GPU trace (Radeon GPU Profiler or the ADR-126 timeline) that shows each
  moved pass overlapping graphics work.
- Equivalent output: `depth` and `gbuffer_*` captures match the inline run.
  `final_color` varies between identical runs and is not an oracle.
- One bounded Debug run with Vulkan synchronization validation, clean.
- The inline fallback passes the same captures with the switch forced on.

No gain is assumed before these results exist.

## Risks

- A missing cross-queue dependency corrupts results or loses the device. The
  2026-10-18 stale-TLAS device loss came from one undeclared dependency on a
  single queue; two queues widen that class of fault.
- GPU hangs on the Windows host have frozen the machine. Diagnose with
  Radeon GPU Detective dumps and ask before runs that may hang.
- Overlapped passes compete for caches and bandwidth; a pass can take longer
  on the compute queue than inline.
- Retained shadows shrink the main window: a still camera skips most shadow
  raster ([ADR-041](../adr/041-retained-cascaded-shadows.md)).
- Per-queue pass times overlap and sum to more than the frame; reports must
  compare frame time, not pass sums.
