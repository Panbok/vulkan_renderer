---
name: vkr-validation
description: Select evidence, justify tests, investigate CPU defects with ASan/UBSan/TSan/MSan/LSan, and run focused native validation or cache checks.
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
| CPU invalid access, uninitialized read, data race, or leak | Select the matching Debug sanitizer profile below; run a reproduction that exercises the suspected defect |
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

Use an existing deterministic CPU test when its independent expected result is
the cheapest way to expose the named failure. Pure algorithms, decoding, stale
handles, and acquire/release contracts often qualify. Before adding a new test,
name the concrete failure, its independent oracle, and why existing build,
harness, or test coverage cannot detect it as directly. A test that copies
implementation logic or asserts a registration constant does not qualify.

```sh
./build_test.sh
./build_test_batch.sh
```

`build_test.sh` incrementally configures the shared `build_debug` tree, builds the
`vulkan_renderer_tester` target, and runs the suite. It does not explicitly
build cooker tools, cook, pack, or bake assets; run Bakery or an explicit
cooker wrapper when fixture regeneration is required. The runner is
`build_debug/tests/vulkan_renderer_tester`; it currently ignores arguments and
has no suite filter. Do not invent a focused-test CLI.

`build_test_batch.sh` configures the shared `build_debug` tree and builds the
test target, then aggregates 50 runs. It does not pack or cook assets; prepare
those inputs explicitly before the batch when a fixture requires them. Use it
only to investigate a specific intermittent failure, not as routine confidence
padding. A failure followed by a pass needs diagnosis; 50 passes cannot prove
the defect absent.
The `.bat` wrappers provide Windows equivalents. With an explicit sanitizer
profile, these wrappers build and run from `build_debug_<profile>` instead;
`VKR_BUILD_DIR` overrides either location.

If a new test is justified, follow the neighboring `tests/src/` file layout and
register its suite in `test_main.c`. CMake discovers source files; it does not
register the suite call. Keep inputs and expected results independent of
scheduling, clock speed, and filesystem ordering.

## CPU sanitizers

Select a profile for a named suspected defect; do not run a sanitizer matrix
after every change. Every explicit diagnostic profile includes UBSan.

| Sanitizer / profile | Use for | Limit on the evidence |
|---|---|---|
| ASan / `address` | Out-of-bounds access, use-after-free and invalid frees | Does not replace MSan or TSan. Check allocator instrumentation for arena/pool defects: backing-allocation bounds alone can miss intra-arena overruns or use after an arena reset. |
| UBSan / included in each profile | Undefined operations such as signed overflow, invalid shifts and misaligned access | The `undefined` group is not every possible UB or integer check. Reports can recover and leave a zero exit status. |
| TSan / `thread` | CPU data races and synchronization visible to the runtime | Only exercised paths/interleavings are covered; a clean run is not proof of race freedom. It does not validate GPU execution or completion. |
| MSan / `memory` | Use of uninitialized CPU data | Requires instrumented dependencies; mixed instrumentation can produce false reports. Use origin tracking to trace the source, not just the reported consumer. |
| LSan / `leak`, or supported `address` runtime | Unreachable heap allocations at leak-check time | Reachable retained memory and allocations hidden inside pools may not be reported. A killed process is not an exit-time leak check. |

Do not combine ASan, TSan and MSan in one executable, or manually append LSan
to TSan/MSan. Use the combinations in
[vkr_sanitizers.cmake](../../../cmake/vkr_sanitizers.cmake), which owns compiler
and runtime support checks; do not bypass a rejected probe to obtain a pass.
Use `VKR_DEBUG_SANITIZER=<profile>` with a build, run or test wrapper; it selects
`build_debug_<profile>` unless `VKR_BUILD_DIR` overrides it. See
[build instructions](../../../docs/INDEX.md#build-and-run) and
[the implemented policy](../../../docs/ARCHITECTURE.md#build-policy) for defaults
and platform limits. Keep compiler, triplet and instrumented dependency changes
in separate trees. Do not reuse a different profile's executable or set
`VKR_BUILD_DIR` to another profile's tree merely to avoid a rebuild.

`default` follows the cached legacy switches; it is not a request for every
sanitizer. New non-Windows Debug trees default to ASan/UBSan, with LSan when
supported; Windows defaults to none. The explicit `none` profile disables CPU
sanitizers. Disable legacy `VKR_ENABLE_SANITIZERS=ON` before selecting an explicit
profile: that switch requests ASan/UBSan in all configurations. Unset
`VKR_DEBUG_SANITIZER` before returning to ordinary Release wrappers. Sanitizer
timings, memory overhead and changed scheduling are diagnostic evidence only;
rerun normal Release for frame-budget or memory-footprint claims.

### Availability and dependency boundaries

Native Windows rejects TSan, MSan and LSan. The Windows address profile requires
the Clang driver, DLL CRT and matching dependencies instead of the default static
CRT; do not mix CRT variants or assume the MSVC/clang-cl driver accepts this
project's combined ASan/UBSan policy. On macOS, TSan and LSan depend on the actual
compiler/runtime installation; AppleClang and upstream LLVM are not interchangeable
evidence. MSan requires Clang on Linux, FreeBSD or NetBSD. A supported operating
system alone does not establish that its C and C++ runtimes link or initialize.

TSan and MSan instrument optimized in-tree vendors. Preserve that exception to
the normal uninstrumented vendor policy: synchronization and memory shadow state
must cross library calls. For the address profile, the current build leaves vendor code
uninstrumented; do not claim vendor access/UB coverage from an instrumented caller.
Record uninstrumented external code relevant to a TSan finding before dismissing
it as a false positive; missing synchronization instrumentation can affect reports.

For MSan, supply an instrumented C++ standard library and external dependencies
through the toolchain before treating diagnostics as evidence. Record their
provenance and investigate uninstrumented library, driver or assembly boundaries
when tracing origins. Do not silence reports by initializing unrelated memory or
adding broad ignorelists. There is no valid full-renderer MSan gate on current
Windows/macOS paths; this build option does not add a Linux renderer. An isolated
fixture can prove its own detection behavior, not full-renderer MSan coverage.

### Run and interpret

Use the smallest existing reproduction that can expose the defect. The CPU test
wrapper still runs the full suite; it has no per-test filter. For a scene-based
reproduction, use a compatible Bistro case and the harness from the selected
profile tree. Serialize GPU runs across profiles and keep the single-process
Metal validation constraints below when combining CPU and native diagnostics.

For fail-fast UBSan diagnostics, use
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`, preserving any required existing
options. For example, on a toolchain supporting TSan:

```sh
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  VKR_DEBUG_SANITIZER=thread ./build_test.sh
```

Inspect stderr and child-process logs even when the suite exits zero. A UBSan
`runtime error:` or another sanitizer finding makes the affected gate fail unless
the fixture deliberately expects that specific report. Fix the first actionable
finding and rerun its reproduction; do not label a crash or startup failure a
clean sanitizer run. Resolve missing symbolizer/runtime libraries with the matching
toolchain before interpreting unsymbolized stacks or changing compiler flags.

For ASan leak checks, first confirm configuration reports
`address,undefined,leak`, then run with `ASAN_OPTIONS=detect_leaks=1` (preserve
other required ASan options). For example:

```sh
ASAN_OPTIONS=detect_leaks=1 VKR_DEBUG_SANITIZER=address ./build_test.sh
```

If the combined LSan runtime is unavailable, use the explicit `leak` profile only
when its runtime is supported. Exercise normal shutdown so threads and owners can
release their allocations and the exit-time check can run. Distinguish leaked
allocations from reachable caches or allocator capacity; use `vkr-memory` and
ownership/accounting evidence for retained-memory growth that LSan cannot see.

Do not add suppressions or disable instrumentation just to make a gate green.
A justified external-runtime suppression must identify the exact finding,
dependency/version, scope and evidence excluded; retain the unsuppressed report.
Record the selected profile, compiler/runtime, target and case, effective runtime
options, relevant dependency instrumentation, exit status and decisive report.
Keep unavailable gates explicit. A successful configure/build or an isolated
runtime probe does not establish a clean application run, GPU safety or parity.

Consult the matching compiler's documentation when a flag or runtime behavior is
uncertain: [ASan](https://clang.llvm.org/docs/AddressSanitizer.html),
[UBSan](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html),
[TSan](https://clang.llvm.org/docs/ThreadSanitizer.html),
[MSan](https://clang.llvm.org/docs/MemorySanitizer.html), and
[LSan](https://clang.llvm.org/docs/LeakSanitizer.html). These upstream descriptions
do not override the narrower toolchain support enforced by this repository.

## Native diagnostics

Use Debug only to reproduce a concrete problem. Run ordinary Release snapshots
and performance separately with validation variables unset.

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
