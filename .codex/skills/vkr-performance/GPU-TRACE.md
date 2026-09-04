# GPU traces

Read this when pass timings locate a cost but encoder attribution or hardware
counters are needed to investigate its cause. An Instruments trace is diagnostic;
matched authoritative harness runs establish performance changes.

## Record a Metal run

Check the installed Xcode tools before recording:

```sh
xcrun xctrace list templates
xcrun xctrace list instruments
```

Require `Metal System Trace` and `Metal GPU Counters`. Ensure the Release
binary/assets are current and validation variables are unset. Choose a short
representative case, check available disk space, and use a new output path.

```sh
xcrun xctrace record --template 'Metal System Trace' \
  --instrument 'Metal GPU Counters' --no-prompt --output /tmp/vkr-audit.trace \
  --launch -- ./build_release/tools/vkr_harness profile \
  --case tools/cases/smoke/sponza_offscreen.case.json \
  --profile tools/profiles/local-offscreen.json
```

The template supplies encoder intervals; the instrument requests counters.
Verify both appeared in the recording. A short time limit can end during asset
loading and capture no frame work. The harness launches renderer children;
check recorded process identities and intervals rather than assuming the parent
alone represents rendering. Native Vulkan does not run on macOS in this tree.

## Export and inspect

```sh
xcrun xctrace export --input /tmp/vkr-audit.trace --toc \
  --output /tmp/vkr-audit-toc.xml
xcrun xctrace export --input /tmp/vkr-audit.trace \
  --xpath '/trace-toc/run[@number="1"]/data/table[@schema="metal-gpu-intervals"]' \
  --output /tmp/vkr-audit-gpu.xml
```

Read the actual table of contents before selecting schemas. Useful schemas
include `metal-gpu-intervals`, `gpu-counter-info`, and `gpu-counter-value` when
present. Resolve XML `id`/`ref` values and inspect units before aggregation.
Pass names may be in `event-label` formatted values rather than object labels.
Filter to verified renderer-child processes; compositor work is separate.

Both backends label graph GPU work. Check current encoder creation sites under
`lib/src/renderer/metal/` and debug-label calls under
`lib/src/renderer/vulkan/` if labels are missing. Preserve cached labels;
do not add per-frame string construction for instrumentation.

| Observation | What to investigate |
|---|---|
| Low occupancy or compiler spills | Register pressure, threadgroup shape, shader live ranges |
| High ALU limiter | Arithmetic and dependency chains |
| High buffer/texture limiter | Access patterns, bandwidth, filtering, cache reuse |
| High write bandwidth | Attachment formats, stores, intermediate images |
| Cache/MMU pressure | Working set and access locality |

Counters identify hypotheses, not guarantees. F16/F32 utilization alone does
not prove that narrower precision is safe or faster. Use `vkr-shaders` for
precision and backend parity decisions, then measure the proposed change.

Per-line shader attribution requires supported Xcode capture/profiling tools;
an empty shader-profiler table supplies no attribution. Verify capability in
the installed tools rather than assuming a trace contains it.

Transcribe the diagnostic finding and recording command, then delete this task's
trace, XML exports, and harness output. Check sizes first; recordings and exports
can each consume hundreds of megabytes.
