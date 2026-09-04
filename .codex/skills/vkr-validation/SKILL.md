---
name: vkr-validation
description: Select evidence for a change, justify tests, diagnose intermittent failures, and run focused native validation or cache checks.
---

# VKR validation

## Choose evidence for the changed invariant

Before running a gate, state what failure it can detect and what result passes.
Use the smallest existing check that exercises that failure. Build success
proves compilation; it does not prove pixels, lifetime, synchronization, or
performance. A documentation-only edit needs structural checks, not a renderer
launch. Use `vkr-harness` for repeatable renderer observations.

| Changed invariant | Evidence to select |
|---|---|
| CPU algorithm, format, allocator ownership | A justified deterministic test or suite under the rule below; otherwise the direct reproduction |
| Renderer output or feature behavior | Small Release harness case and affected captures/assertions |
| Native commands, resource transitions, GPU lifetime | Small reproduction under the affected backend's native validation; inspect diagnostics and execution result |
| Shader math, bindings, dispatch, host ABI | `vkr-shaders` parity gates, including compiled contracts and affected native cases |
| Pipeline cache persistence | Isolated cold/prewarm/warm execution with actual cache load/save evidence |
| Threading, queues, slot reuse, target recreation | Case that exercises the changed state transition or lifetime, plus native synchronization validation |
| Frame cost or memory efficiency claim | Correctness evidence above as applicable, then matched Release measurements from `vkr-performance` |

A renderer change does not automatically require the CPU suite. There is no
current `tools/validate_multithreaded_backend_matrix.sh`; choose cases from
`tools/cases/` that exercise the affected behavior. Inspect their assertions
before treating a filename as coverage. If the existing tooling cannot exercise
the required invariant, identify the missing mechanism immediately and resolve
that decision with the user before dependent implementation.

## CPU tests

Default to no unit-test work. Before adding or running a test or suite, name
the concrete failure, an expected result independent of the implementation,
and why a build, harness run, or existing check cannot detect it as directly
or cheaply. Use a test only when it detects that failure deterministically
at the responsible boundary. Pure algorithms, decoding, stale handles, and acquire/release
contracts can qualify. A test that copies implementation logic or asserts a
registration constant does not.

```sh
./build_test.sh
./build_test_batch.sh
```

`build_test.sh` freshly configures `build_test`, builds the CPU test runner and
font cooker, checks deterministic font output and unchanged-output skipping,
cooks required assets, packs textures, and runs the suite. The runner is
`build_test/tests/vulkan_renderer_tester`; it currently ignores arguments and
has no suite filter. Do not invent a focused-test CLI.

`build_test_batch.sh` requires the configured `build_test` tree, rebuilds the
test target, packs once, then aggregates 50 runs. Use it only to investigate a
specific intermittent failure, not as routine confidence padding. A failure
followed by a pass needs diagnosis; 50 passes cannot prove the defect absent.
The `.bat` wrappers provide Windows equivalents.

If a new test is justified, follow the neighboring `tests/src/` file layout and
register its suite in `test_main.c`. CMake discovers source files; it does not
register the suite call. Keep inputs and expected results independent of
scheduling, clock speed, and filesystem ordering.

## Native diagnostics

Use Debug only to reproduce a concrete problem. Supported non-Windows Debug
builds enable ASan/UBSan by default. Run ordinary Release snapshots and
performance separately with validation variables unset.

On Windows, `build.bat Debug` enables Vulkan validation through the backend's
Debug configuration, including synchronization checks. Run the smallest relevant
case using `build_debug/tools/vkr_harness.exe` or its `Debug/` multi-config path.
Require actual `VK_LAYER_KHRONOS_validation` initialization and inspect child
stdout/stderr for validation errors. A native Vulkan run is unavailable on
macOS in the current implementation; a Metal pass cannot close that gate.

For Metal API validation, set `MTL_DEBUG_LAYER=1` before device creation. Add
`MTL_SHADER_VALIDATION=1` only when shader/GPU diagnosis is needed. Check live
processes first:

```sh
pgrep -fl 'vkr_harness|vulkan_renderer'
```

Wait for an existing renderer or coordinate with its owner before launching.
Identify unknown harness processes; do not terminate another task's processes.
Exactly one validation-enabled process may create/use a Metal device at a time.
A harness parent can supervise one child; do not overlap renderers, repetitions,
matrices, or capture workers. Broad validation capture suites are prohibited:
a validation-enabled multi-capture run was followed by a macOS watchdog panic;
the cause was not established.

For a layered-transmission issue, this existing bounded case is an example:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  ./build_debug/tools/vkr_harness profile \
  --case tools/cases/local/p18_metal_dual_validation_serial.case.json \
  --profile tools/profiles/local-metal-dual-validation-serial.json
```

Build first with `./build.sh Debug` if needed. The example has one repetition;
select another minimal case when transmission does not exercise the issue.
MetalFX can be unavailable under Apple's validation wrappers; report the actual
configuration and do not treat a fallback run as MetalFX evidence.

## Pipeline cache check

Use a minimal harness case with `cache=isolated_warm`. The harness owns an
isolated cache path and launches a prewarm child before measured children.
Inspect logs and cache files: the fresh prewarm must save nonempty data; the
next child must load that data and save successfully. Record byte counts,
backend, configuration, and any validation errors. `cache=isolated_cold` alone
cannot prove persistence across processes. Do not infer cache success merely
from a passing profile or use the retired `validate_pipeline_cache.sh`.

## Completion

Record exact commands, passing assertions, diagnostic findings, and unavailable
coverage. Fix a failure before rerunning its affected check. Repeat or broaden
checks only for new edits, failures, or an unresolved invariant. Native backend
coverage, pixel equivalence, and performance authority are separate results.
Retire this task's run output using `vkr-harness` after recording its evidence.
