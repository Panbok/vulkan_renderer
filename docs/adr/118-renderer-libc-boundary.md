---
status: implemented
updated: 2026-10-14
authority: adr
---

# ADR-118: Renderer and runtime libc and platform boundary

## Status

Accepted. `renderer_lib`, `vkr_render_contracts`, `vkr_runtime`,
`vkr_sample_runtime`, `vkr_asset_formats` and `vkr_image_decode` reach the
C library and the operating system only through `vkr_foundation`
(`lib/src/`). The compiler enforces the boundary for stdio, stdlib, string,
POSIX and Win32 declarations in their C and Objective-C units; a source
guard covers `<math.h>`, Objective-C, vendor headers and C++ units. macOS
(Metal) has native evidence; the Windows build, the Win32 surface and the
runtime's Windows platform units are unrun.

## Context

`renderer/CMakeLists.txt` precompiled `lib/src/vkr_pch.h` into
`renderer_lib`. That header includes the whole C library, `<pthread.h>`,
`<unistd.h>`, `<mach/mach_time.h>` and `<windows.h>`, so every renderer
file could call any of them without an include. An audit on 2026-10-14
found 494 such lines in 46 files: `<math.h>` calls, `snprintf`, `getenv`,
stdio and POSIX file I/O, `malloc`, `CACurrentMediaTime`, GCD and
`NSFileManager`. Two were Windows defects. `getenv` and `fopen` read the
pipeline cache and shader catalog paths in the active code page
([ADR-070](070-portable-path-boundaries.md)). The runtime had the same
pattern on a larger scale: 2,351 uses across `isfinite`, `snprintf`,
`strcmp`, `malloc`, stdio file I/O, `getenv`, `qsort` and the `strto*`
parsers.

## Decision

1. **Freestanding includes.** Renderer and runtime targets define
   `VKR_LIBC_BOUNDARY` for their C and Objective-C sources, and the renderer
   no longer precompiles `vkr_pch.h`. Under that define
   [defines.h](../../lib/src/defines.h) includes only `<stdarg.h>`,
   `<stdbool.h>`, `<stddef.h>`, `<stdint.h>` and the SIMD intrinsic headers,
   and `vkr_pch.h` is an error. `MemCopy`, `MemSet`, `MemZero` and
   `MemCompare` use compiler builtins on Clang and GCC; MSVC `cl` includes
   `<string.h>` for them. Lib headers that the renderer includes carry no
   stdio or OS declaration: `file_fopen` and `file_flush_durable` are
   hidden with the `FSEEK64` macros, `VkrJsonFileWriter` holds its stream
   as `void *`, and header assertions use `vkr_assert`. Other targets (the
   editor, app, tools and tests) keep `vkr_pch.h` through `defines.h`.
2. **Math.** [vkr_math.h](../../lib/src/math/vkr_math.h) is the one lib
   header that includes `<math.h>`, for its inline wrappers. Each wrapper
   calls the libm function of the same precision and semantics, so a
   migrated call produces the same bits: `vkr_fmin_f32` and `vkr_fmax_f32`
   keep IEEE NaN handling, unlike `vkr_min_f32`. `vkr_is_finite_f32`,
   `vec3_is_finite`, `vec4_is_finite` and `mat4_is_finite` replace
   `isfinite`. `VKR_FLOAT_INFINITY` and `VKR_FLOAT_NAN` replace the C macros,
   and the `INFINITY` fallback of `1e30f` is gone.
3. **Lib owners.** Code behind the boundary uses `string_format`,
   `string_equals`, `string_compare`, `string_length` and
   `string_length_bounded` for text, and `string_parse_f64`/`_f32`/`_i64`/
   `_u64` where the C library's leading-number semantics and end pointer
   matter; `vkr_platform_environment_variable`, `_flag` and `_equals` for
   the environment; `FileHandle` with `file_path_view`, `file_seek`,
   `file_remaining_size`, `file_truncate` and `file_remove_directory` for
   files; `vkr_platform_get_absolute_time` for time;
   `vkr_platform_stdout_write`, `vkr_platform_stderr_write` and their `f`
   forms for process output; `vkr_platform_exit_immediate`; `vkr_sort` and
   `vkr_binary_search`; `MemFindByte`; and
   [vkr_heap.h](../../lib/src/memory/vkr_heap.h) for blocks whose owner
   changes across modules and threads. Texture upload payloads are such
   blocks, and vendor decoders allocate them with `malloc`, so the heap
   functions are the C heap itself. `VKR_CHAR_BITS` and
   `VKR_F32_LIMITS_BINARY32` in `vkr_byte_io.h` state the cooked formats'
   byte and float assumptions.
4. **Background pipeline creation.** Custom graph and tiled BRDF variant
   pipelines are jobs on the runtime's job system, passed as
   `VkrRendererBackendConfig.job_system`. Without one, or with no free job
   slot, the requesting thread creates them. The job system's owner calls
   `vkr_renderer_detach_jobs` with no frame in flight before it shuts the
   job system down; the call waits for every renderer job.
5. **Platform units.** Metal APIs take GCD and Blocks types:
   `MTLSharedEventListener` needs a dispatch queue, and MTL4 commit feedback
   needs a Block handler drained through a serial queue. Those calls stay in
   the Metal backend, beside `NSString`, `NSURL` and `NSError` at Metal API
   calls. [vkr_vulkan_wsi.c](../../renderer/src/vulkan/vkr_vulkan_wsi.c) is
   the only renderer file that includes `<windows.h>`, for the Win32
   surface. `runtime/src/platform/` is the runtime's window, gamepad and
   file-dialog layer: it includes AppKit, GameController, Win32, XInput and
   DXGI headers and calls their APIs, its Windows units add `<windows.h>`
   and `<wchar.h>` for themselves, and its C library calls go through the
   lib. `PLATFORM_APPLE` and `PLATFORM_WINDOWS` remain build switches that
   select a backend or platform unit; they are not OS calls.
6. **C++ adapters.** `vkr_physics.cpp` (Jolt) and `vkr_mesh_decode.cpp`
   keep the C++ standard library, which their vendor APIs need, and stay
   outside `VKR_LIBC_BOUNDARY`. Their C library calls use the lib
   wrappers.
7. **Guard.** [check_libc_boundary.py](../../tools/checks/check_libc_boundary.py)
   runs in `build_test.sh` and `build_test.bat` over `renderer/src` and
   `runtime/src`, skipping vendor implementation units. After it strips
   comments and literals, it rejects C library calls and limit macros,
   C library headers, and operating-system headers and calls outside the
   platform units. Its exception table names each API-required GCD call
   and the WSI and Windows platform includes.

## Consequences

- A new renderer or runtime use of stdio, stdlib, string, POSIX or Win32
  fails to compile on Clang. A new `<math.h>` call, a Foundation-provided
  call in an Objective-C unit, or a C library call in a C++ adapter fails
  the guard.
- A need for a new C library or OS service adds a lib function first.
- An environment variable set to an empty string reads as unset, since
  `vkr_platform_environment_variable` cannot tell them apart.
- Metal diagnostics directories and segments take the lib's default
  permissions (0755 and 0644) instead of 0700 and 0600.

## Evidence

On 2026-10-14 the change ran on an M1 Pro, Release, Metal. All 79 renderer
translation units compiled under `VKR_LIBC_BOUNDARY`, and the app, harness,
editor, player and CPU tester built with warnings as errors. The tester
passed the same 810 tests as its base. `bistro_retained_shadow_snapshot`
(final color and four cascades) and `bistro_editor_views` (four channels)
gave identical `data_sha256` before and after. A temporary Bistro case with
a BRDF-extension cube built the tiled extension variant through the job
system and matched the GCD build after 150 warm-up frames. The guard
reported 651 hits on the unmigrated sources and none after.

The runtime change ran on the same machine and configuration from base
`3dba57ab`. All 115 runtime units compiled under `VKR_LIBC_BOUNDARY`, and
every target built with warnings as errors. The tester passed the same 789
named tests with the same logged errors and warnings, including the
heightfield file, journal and streamed tests, the scene-edit overlay and
the collision asset tests. `bistro_retained_shadow_snapshot`,
`bistro_editor_views` and `bistro_windowed_snapshot` gave identical
`data_sha256` for all ten captures before and after. A headless Bistro
editor session with `VKR_EDITOR_LAYOUT_PATH` and
`VKR_GRAPHICS_SETTINGS_PATH` set wrote its dock layout, and a second
session loaded it. The guard reported 2,351 runtime hits before and none
after.

## Alternatives considered

- **Out-of-line math on MSVC to drop `<math.h>`.** This adds a call per
  operation in per-frame validation on MSVC builds; the guard covers the
  remaining gap.
- **A source guard alone.** This keeps `vkr_pch.h`, so new uses still
  compile; the compiler boundary catches them at build time.
- **GCD or a dedicated renderer thread for background pipelines.** The
  owner chose the runtime job system on 2026-10-14.

## Revisit when

The editor, app or tools take the same boundary, or the Windows build
shows a header that `VKR_LIBC_BOUNDARY` misses under MSVC or clang-cl.
