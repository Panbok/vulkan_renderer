---
status: superseded
updated: 2026-07-31
authority: progress
---

> **Archived.** Superseded by [`../../.codex/skills/vkr-validation/SKILL.md`](../../.codex/skills/vkr-validation/SKILL.md). Retained for history; do not treat as current.
# Multithreaded Vulkan Backend Validation Matrix

This document defines the reproducible validation matrix for the active
parallel-upload rollout gates.

Render-record parallelization has been retired from the active implementation
track, so validation now covers serial vs upload-parallel behavior only.

## Runner

- Script: `tools/validate_multithreaded_backend_matrix.sh`
- Logs directory:
  `build/_validation/multithreaded_backend/logs`

## Matrix Coverage

### Compile-time macro coverage

- `default_compile`:
  uses repository defaults (`VKR_VULKAN_PARALLEL_UPLOAD=1`)
- `compile_serial`:
  forces `-DVKR_VULKAN_PARALLEL_UPLOAD=0`
- `compile_upload_parallel`:
  forces `-DVKR_VULKAN_PARALLEL_UPLOAD=1`

### Runtime env coverage (executed against `default_compile`)

- `default_env` (no env overrides)
- `force_serial`:
  `VKR_PARALLEL_UPLOAD=0`
- `upload_only`:
  `VKR_PARALLEL_UPLOAD=1`

## Commands

Smoke check:

```bash
tools/validate_multithreaded_backend_matrix.sh --smoke
```

Full matrix:

```bash
tools/validate_multithreaded_backend_matrix.sh
```

## Latest Run (2026-02-09)

- Smoke: `PASS=1`, `FAIL=0`
- Full matrix: `PASS=5`, `FAIL=0`

## Notes

- The runner builds `vulkan_renderer_tester` and executes the full test suite
  per matrix case.
- Any failing case returns non-zero and prints the failing log path.
