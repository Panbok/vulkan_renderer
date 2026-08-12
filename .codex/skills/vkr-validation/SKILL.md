---
name: vkr-validation
description: Test and validation gates for the VKR renderer. Use when deciding whether to add a unit test, running the test suite, hunting an intermittent failure, running Vulkan validation layers, validating the pipeline cache or the multithreaded backend matrix, or determining what evidence a change needs before it is done.
---

# VKR Validation

## Choose the cheapest gate that completely covers the invariant

Running everything is slow and running nothing is worse. Pick by what you
touched.

| You changed | Minimum gate |
|---|---|
| Math, containers, memory, ECS, JSON, string, filesystem | Focused test + `./build_test.sh` |
| A renderer subsystem with CPU-observable behavior | `./build_test.sh` |
| Anything that records Vulkan commands or touches resource state | `./build_test.sh` + Vulkan validation-layer run |
| Shaders, reflection, pipeline creation | Explicit cold/warm production cache run + validation-layer run |
| Backend threading, command pools, queue use | `tools/validate_multithreaded_backend_matrix.sh` |
| A hot path, batching, culling, upload path | All of the above + a Release measurement (`vkr-performance`) |
| A test that failed once and then passed | `./build_test_batch.sh` |

## Commands

```sh
./build_test.sh          # build + run the CPU suite (build/tests/vulkan_renderer_tester)
./build_test_batch.sh    # 50 consecutive runs; use to confirm or refute a flake
tools/validate_multithreaded_backend_matrix.sh     # backend threading matrix
tools/validate_multithreaded_backend_matrix.sh --smoke
```

`./build_test.sh` configures with `--fresh`, so it is a clean-configure gate,
not an incremental one.

For a pipeline-cache gate, run the normal application twice with the same fresh
explicit `VKR_PIPELINE_CACHE_PATH` and a bounded auto-close. Require the first
run to initialize empty and save a non-empty cache, the second to load that
cache and save successfully, and both runs to complete without validation
errors. Record the cache byte counts and target configuration. The retired
`validate_pipeline_cache.sh` depended on legacy `pipeline.create` events and is
not a current command.

## The CPU suite does not replace a validation-layer run

The test runner under `tests/src/` is predominantly CPU-side. It does not
exercise:

- multiple swapchain image counts (the frame-stream indexing gap in
  `docs/architecture/renderer-architecture-spec.md` §7.4 only appears at four
  images);
- different queue-family layouts (separate transfer queue present or absent);
- different GPUs and drivers;
- resize and device-loss scenarios;
- actual barrier and layout-transition correctness.

For anything that records commands or transitions resources, run the app under
the Vulkan validation layers and read the output. A green CPU suite is not
evidence that the Vulkan usage is correct.

## When to add a unit test

Be deliberate. There are already 40+ test files under `tests/src/`; adding a
weak one costs maintenance forever and proves little.

Add a test when it covers a **critical invariant completely and
deterministically at the right seam**:

- pure algorithms — math, frustum, transform, string, JSON, bitset;
- lifecycle and ownership contracts — acquire/release symmetry, allocator
  accounting across a load/unload cycle;
- handle generation and stale-use rejection;
- schema and format decoding — glTF, KTX2/`.vkt`, texture format selection,
  reflection-driven pipeline layout;
- cache behaviour;
- deterministic state machines — async resource state transitions, batching
  decisions.

Do **not** add a test that merely mirrors implementation structure, restates a
pass registration, or asserts on values a Vulkan run owns (pixels, timings,
barrier emission). Those belong to a validation-layer run or a measurement.

Before adding one, state why the failure it targets cannot be caught more
directly by the existing suite, a validation-layer run, or a measurement.

## Test conventions

- Files live in `tests/src/` and are auto-included by CMake — no registration
  edit needed.
- Name them `*_test.c` / `*_test.h` or `*_tests.c` / `*_tests.h`, matching the
  existing split.
- Register the suite from `test_main.c` following the neighbouring entries.
- Keep tests deterministic. A test that depends on timing, thread scheduling, or
  filesystem ordering will become a flake, and a flake is worse than no test.

## Intermittent failures

A test that fails once is not noise until proven. Run `./build_test_batch.sh`
(50 iterations) before dismissing it. If it reproduces at any rate, treat it as
a real bug — the job system, event system, and async resource pump are all
genuinely concurrent here, and an intermittent failure in any of them is a
lifetime or synchronization defect, not test flakiness.

## Reporting

State what you ran, what passed, and what you did not run. If a gate was skipped
because the environment could not support it (no second GPU, no separate
transfer queue), say so explicitly rather than implying coverage.

Never report a change as done on the strength of a build succeeding.
