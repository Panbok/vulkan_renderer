# GPU traces

A **GPU trace** is an Instruments recording of one harness run, driven entirely
from the CLI. It answers the question per-pass timestamps cannot: *why* a pass
costs what it costs — which hardware limiter it hits, what its occupancy is, how
much bandwidth it moves.

Standing as evidence: a trace **localizes and explains**; it never establishes a
speed claim. Instruments perturbs the command stream, and one recording is one
process. The authoritative number stays `vkr_harness profile` with
`performance-windowed-gpu.json`. Use a trace to decide what to change, then
prove the change with the harness.

Metal only. The Vulkan implementation returns
`initialization_supported = false` outside Windows (`vkr_renderer_impl.c`), so a
macOS trace always records the Metal path.

## Prerequisite: labelled passes

Both backends name their GPU work from the graph pass name, so every encoder in
a trace is attributable to a pass:

- Metal sets `encoder.label` at both encoder creation sites in the pass loop
  (`vkr_metal_packet_frame.inc`), from an `NSString` cached per pass index.
- Vulkan brackets each pass with `vkCmdBeginDebugUtilsLabelEXT` /
  `EndDebugUtilsLabelEXT` (`vkr_vulkan_graph.c`), with
  `VK_EXT_debug_utils` enabled whenever present rather than only under
  validation.

A new encoder creation site that skips the label reappears in traces as
`Compute Command 7` and silently loses attribution. Label it.

## Record

```sh
./build_release.sh
xcrun xctrace record --template 'Metal System Trace' --instrument 'Metal GPU Counters' \
  --no-prompt --output /tmp/vkr.trace \
  --launch -- ./build_release/tools/vkr_harness profile \
    --case tools/cases/smoke/sponza_offscreen.case.json \
    --profile tools/profiles/local-offscreen.json
```

Both flags are load-bearing, and neither alone is enough:

- `--template 'Metal System Trace'` supplies the `metal-gpu-intervals` table —
  per-encoder durations carrying the pass label. Alone it records
  `Counter Set: (null)` and yields zero counter rows.
- `--instrument 'Metal GPU Counters'` flips the recording to
  `Counter Set: Performance Limiters` and `Shader Timeline: Enabled`. Alone it
  records against the **Blank** template, which has no `metal-gpu-intervals`
  table at all — counters with no pass attribution.

Together they produce one recording with both.

The harness forks a child process per repetition and those children are
recorded, so no special handling is needed.

## Export

```sh
xcrun xctrace export --input /tmp/vkr.trace --toc          # schema inventory
xcrun xctrace export --input /tmp/vkr.trace \
  --xpath '/trace-toc/run[@number="1"]/data/table[@schema="metal-gpu-intervals"]'
```

Schemas worth naming: `metal-gpu-intervals` (per-encoder duration plus the pass
label), `gpu-counter-info` (counter definitions), `gpu-counter-value` (samples).

Two traps in the exported XML. Pass labels live in the `event-label`
formatted-label field, **not** in the `metal-object-label` column, which stays
empty. And the XML deduplicates values by `id`/`ref`, so a row often carries
`<duration ref="2"/>` instead of a number; resolving refs is required:

```python
import xml.etree.ElementTree as ET, collections
vals = {}
def val(el):
    if el.get('ref') is not None: return vals.get(el.get('ref'))
    if el.tag == 'formatted-label':
        s = el.find('string'); v = val(s) if s is not None else None
    else:
        v = el.text if el.tag == 'duration' else el.get('fmt')
    if el.get('id') is not None: vals[el.get('id')] = v
    return v

agg = collections.defaultdict(lambda: [0, 0.0])
for row in ET.parse('gpu.xml').getroot().iter('row'):
    name = dur = None
    for ch in list(row):
        v = val(ch)
        if ch.tag == 'duration' and dur is None: dur = v
        if ch.tag == 'formatted-label' and name is None: name = v
    if name and dur is not None:
        agg[name][0] += 1; agg[name][1] += float(dur) / 1e6
for n, (c, ms) in sorted(agg.items(), key=lambda r: -r[1][1])[:25]:
    print(f'{n[:44]:<45}{c:>7}{ms:>10.2f}ms{ms*1000/c:>9.1f}us')
```

Rows naming processes other than `vkr_harness` are the compositor and whatever
else holds the GPU; ignore them.

## Read

Counters map to the question you are asking:

| Question | Counters |
|---|---|
| Is the kernel register-bound? | Compute / Fragment / Vertex Occupancy |
| Arithmetic or memory bound? | ALU Limiter vs Buffer Read/Write Limiter |
| Is a G-buffer write the cost? | GPU Write Bandwidth, Texture Write Limiter |
| Is sampling the cost? | Texture Sample / Filtering / Cache Limiter |
| Is early-Z being lost? | Fragment Occupancy, Fragment Input Interpolation Limiter |
| Cache or address translation thrashing? | GPU Last Level Cache Limiter, MMU Limiter, MMU TLB Miss Rate |

`F16 Utilization` against `F32 Utilization` shows whether a kernel would gain
from narrower precision. `graphics-compiler-spill-events` reports register
spills when the compiler emits them.

## What a trace does not give

Per-source-line shader cost. The four shader-profiler schemas
(`metal-shader-profiler-intervals`, `metal-shader-profiler-shader-list`,
`gpu-shader-profiler-sample`, `gpu-shader-profiler-interval`) export zero rows
even with `Shader Timeline: Enabled`, because the Shader Profiler is an Xcode
GPU-frame-capture feature rather than an Instruments one. Reaching it needs a
`.gputrace` opened in the Xcode GUI, and the repository has no
`MTLCaptureManager` hook to produce one. Treat per-line attribution as
unavailable until someone adds that hook and opens the result by hand.

## Gotchas

- **Traces are large, and exports are larger.** A full Sponza offscreen run
  records a 650 MB trace, and exporting `metal-gpu-intervals` alone writes a
  188 MB XML file. Check free space before recording and delete both once you
  have the numbers; filling the disk mid-session wedges every tool that writes.
- **Boot dominates the front of a run.** A short `--time-limit` records zero GPU
  intervals because asset loading finishes long before the render loop starts.
  Let the run exit on its own, or set a limit past boot.
- **Compare trace to trace.** Instruments changes what it measures, exactly as
  the timestamp-on harness profile does.
