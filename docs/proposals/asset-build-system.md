---
status: proposed
updated: 2026-09-29
authority: proposal
---
# Asset build system specification

This document specifies `vkr_bakery`, one native build system that owns asset
cooking, shader compilation, caching, scheduling, progress, diagnostics, editor
integration, script compilation and game bundling. Phases 0 to 5 shipped on
macOS and are recorded in [ADR-077](../adr/077-asset-build-system.md), which
owns their contract, including where the daemon, Content state and bundles
deviate from sections 10 and 11; sections 2 through 12 remain the
specification that the unshipped work extends. Remaining scope: the daemon's
Windows transport, Windows Vulkan bundles and script modules, shader hot
reload, and mesh-cooker derived textures as child `texture` actions (declined
in ADR-077 until a second consumer needs them). Loading script modules belongs
to the entity behavior proposal. Bundles of managed projects, the game
executable and the editor's Build workflow shipped as
[ADR-078](../adr/078-project-build-and-packaging.md), which supersedes the
`bundle <project>` form in sections 5 and 11; their remaining scope is the
[project build and packaging proposal](project-packaging.md).

## Handoff notes for the implementer

- Load `vkr-task-workflow`, then the domain skill for each phase:
  `vkr-shaders` for the shader producer and every runtime shader-loading
  change, `vkr-memory` for the cache and scheduler, `vkr-renderer-design` for
  the runtime virtual filesystem, `vkr-docs` when a phase ships.
- The user accepted the recommendations in section 14 as defaults. Do not
  reopen them without new evidence; ask only when an item marked "ask" arises.
- Bistro is the only scene for measurements, imports and bundle tests.
- This host has 16 GiB. Never run cookers in an unbounded loop; the memory
  budget in section 8 is a hard requirement from the first parallel phase.
- Every speed claim needs a matched Release before/after measurement with the
  per-action event log retained. Section 13 names the baseline each phase
  compares against.
- The Metal offline toolchain is installed here: `xcrun -sdk macosx metal
  --version` reports Apple metal 32023.921 targeting `air64-apple-darwin25.6.0`
  and `metallib` is present. Section 9.7 makes `.metallib` production a normal
  Metal action on macOS; a host without the toolchain gets a warning and the
  source-compile fallback, and phase 1 acceptance on this host requires the
  compiled libraries.

## 1. Baseline

[ADR-077](../adr/077-asset-build-system.md) describes the shipped `vkr_bakery`:
producers, the per-user action cache, the scheduler, the event stream, the
shader catalog with Metal metallibs, `project` jobs, `bake diffuse|probe` and
`preview material|prune`. The editor launches only `vkr_bakery`. Before it, one
4096² PNG took 5.97 s wall (30.2 s CPU) to encode on the M1 host; Bistro
references 686 images, so UASTC encoding and cache misses dominate import time.

### 1.5 Engine survey

- **Unreal Engine 5.** The Derived Data Cache stores expensive derived data
  under a key of data-type version plus input hashes across local, shared and
  cloud stores fronted by the Zen server; cooking runs in worker processes,
  shaders compile in `ShaderCompileWorker` processes, and asset compilation
  shows placeholders while final builds finish. Cooked data ships in IoStore
  containers addressed by chunk.
- **Unity.** AssetDatabase v2 keys artifacts by source hash, `.meta` settings,
  importer version, platform and declared dependencies; the Accelerator shares
  them; textures and models import in worker processes; the Import Activity
  window shows per-asset time and reasons. Bee builds players; the Scriptable
  Build Pipeline and Addressables package content.
- **Godot 4.** `.import` sidecars beside sources, `.godot/imported/` outputs,
  a scan comparing modification time and MD5, export presets that pick a
  prebuilt template and pack a `.pck`, and a headless export command.
- **Frostbite.** Publicly described as a data-driven pipeline with per-type
  builders, incremental dependency-tracked and distributed builds with a
  shared cache, and shipped data in content-addressed `.cas` chunks with
  `.cat` catalogs grouped into bundles and superbundles.
- **O3DE, Bazel, Buck2, Ninja.** A watching daemon with builder plugins and an
  asset catalog; actions keyed by inputs, command and environment with
  content-addressed outputs and a structured event stream shared by console
  and IDE front ends; depfiles for dependencies discovered while a tool runs.

The mechanisms behind "fast" are the same everywhere: never redo an action
whose key exists, run independent actions in parallel, and emit one event
stream that every front end consumes.

## 2. Goals and non-goals

Goals:

1. One binary and one CLI vocabulary for every producer, including shaders.
2. A dependency graph with content-addressed caching so a warm build performs
   no work and a cold build runs every independent action in parallel under a
   memory budget.
3. One structured event stream with per-action progress, coded diagnostics and
   timing, consumed by the terminal, the editor Console and the persisted log.
4. Shaders compiled on demand for one backend, one entry or one changed file,
   loaded by the runtime through a catalog instead of compile-time paths, and
   shipped as data.
5. Editor integration without Python and without build-tree paths.
6. A bundle command that produces a runnable game directory from a project.
7. Script sources compiled and bundled through the same graph.

Non-goals for this specification: distributed or remote execution, a shared
network cache, a scripting runtime design (owned by the entity behavior
proposal), changes to any published artifact format, and any change to what
ADR-044 requires as native shader validation.

## 3. Concepts

| Term | Definition |
|---|---|
| Source | A file under the project, repository or an explicit include root, identified by portable path and content hash. |
| Recipe | The validated import settings for one source or one derived request, as a JSON record beside the source (`<source>.recipe.json`) or supplied inline on the command line. |
| Producer | A compiled-in component that declares a stable `id`, a `version` integer, the recipe schema it accepts, its static inputs, and a function that runs one action and reports discovered inputs. |
| Action | `(producer id, producer version, canonical recipe, sorted input hashes, platform, tool identity)`. Its key is the SHA-256 of the canonical JSON encoding of that tuple. |
| Product | An immutable output stored once in the content-addressed store and referenced by hash. An action produces one or more products plus a result record. |
| Depfile | The list of inputs a producer discovered while running, recorded in the action result and folded into the key of the next run. |
| Root set | The sources a `build` or `bundle` starts from: a project's scenes, fonts, bakes, shader libraries and scripts, or an explicit list. |
| Catalog | The mapping from stable asset identities to product hashes that the runtime loads. In the editor it is a JSON file in the workspace; in a bundle it is the `.vkpak` header. |

Producer version increments whenever the producer's output bytes for the same
inputs could change. The build of `vkr_bakery` also embeds a tool identity
(git revision or a content hash of the binary) that participates in the key,
so a rebuilt tool with an unchanged producer version still revalidates.

## 4. Binary and source layout

`vkr_bakery` is one C11 executable under `tools/bakery/` linking the existing
libraries `vkr_asset_cooking`, `vkr_vkt_cooking`, `vkr_collision_cooking`,
`vkr_bake_core`, the font encoder and the table generators in process. The
present executables remain until the phase that replaces them ships and their
callers are retired in the same change.

```
tools/bakery/
  vkr_bakery_main.c        argv parsing, subcommand table, exit codes
  vkr_bakery_cli.c/.h      shared flags, help rendering, terminal renderer
  vkr_bakery_events.c/.h   event stream writer, log file, JSON schema versions
  vkr_bakery_diag.c/.h     diagnostic codes and formatting
  vkr_bakery_index.c/.h    source index: path, size, mtime, inode, hash
  vkr_bakery_recipe.c/.h   recipe loading, normalization, canonical encoding
  vkr_bakery_graph.c/.h    root-set walk, action key derivation, depfile merge
  vkr_bakery_cache.c/.h    CAS, action index, locks, garbage collection
  vkr_bakery_sched.c/.h    job-system scheduling, memory budget, cancellation
  vkr_bakery_worker.c      isolated action runner (subcommand `worker`)
  vkr_bakery_serve.c       editor daemon (phase 3)
  vkr_bakery_bundle.c/.h   .vkpak writer (phase 4)
  producers/
    producer.h             VkrBakeryProducer interface
    texture.c mesh.c font.c animation.c collision.c table.c
    diffuse_volume.c reflection_probe.c shader.c script.c project.c
```

New source files are justified by independent responsibilities and lifetimes:
the cache outlives any build, the scheduler owns threads, producers are the
extension boundary, and the daemon has its own process lifetime. Helpers stay
internal until a second translation unit needs them.

The runtime gains one small library, `vkr_vfs` under `runtime/src/vfs/`, that
mounts loose directories and `.vkpak` archives behind the loaders (phase 4).
The shader catalog reader used by both backends lives in
`renderer/src/vkr_shader_catalog.c/.h` (phase 1).

## 5. CLI contract

```
vkr_bakery <subcommand> [options] [arguments]

  cook     <source>... [--recipe k=v]... [--out <path>]      cook listed sources, any kind
  build    <project> [--scene <id>]... [--platform <p>]       build the project's root set
  shaders  [--backend vulkan|metal|all] [--entry <name>]...   compile shader libraries
           [--library <path>] [--watch]
  import   <project> <source>...                              managed import (replaces project jobs)
  inspect  <artifact|source>                                  existing --inspect modes, recipe view
  explain  <artifact|action-key>                              why it ran, inputs, timing, products
  status   <project>                                          stale, missing and failed actions
  gc       [--cache <dir>] [--older-than <days>] [--dry-run]  cache garbage collection
  bundle   <project> --platform <p> --config <c> --out <dir>  produce a runnable game directory
  serve    <project> [--socket <path>]                        editor daemon
  worker   --action <key> --request <fd|path>                 isolated single-action runner
  help     [<subcommand>]
```

Shared options accepted by every subcommand:

| Option | Meaning |
|---|---|
| `--json` | Emit the event stream on stdout; human output moves to stderr. |
| `--log <file>` | Append the event stream to a file. Default: `<workspace>/.vkreditor/logs/bakery-<utc>.jsonl` when a project is known, else none. |
| `--jobs <n>` | Worker count. Default: hardware threads. |
| `--memory-budget <MiB>` | Upper bound on the sum of running actions' declared peak memory. Default: half of physical memory. |
| `--cache <dir>` | Cache root. Default: `$VKR_BAKERY_CACHE` or the per-user cache directory (`~/Library/Caches/vkr_bakery` on macOS, `%LOCALAPPDATA%\vkr_bakery` on Windows). |
| `--platform <p>` | `host`, `macos-arm64`, `windows-x64`. Default: `host`. |
| `--force` | Ignore cache hits for the actions named on the command line only, not their dependencies. |
| `--dry-run` | Build the graph and print what would run without running. |
| `--quiet`, `--verbose` | Terminal renderer verbosity; do not affect `--json`. |

Exit codes: `0` success, `1` at least one action failed with reported
diagnostics, `2` usage or recipe validation error, `3` cancelled, `4`
environment error such as a missing SDK or unwritable cache. A subcommand never
prints a stack of unrelated errors; the first usage error ends the run.

Every producer-specific setting is a recipe field, never a flag. `--recipe`
overrides a field for the sources on the command line. `vkr_bakery help <sub>`
prints options, recipe fields for the producers involved, exit codes and one
example. Paths on the command line and in events are UTF-8 with `/` separators
and follow ADR-070.

## 6. Event stream

One line-delimited JSON object per event. Every event carries `"v": 1` and a
monotonic `"t"` in milliseconds since the run started. Unknown fields are
ignored by readers; a new field never changes an existing field's meaning; a
breaking change increments `v`.

| `ev` | Fields | When |
|---|---|---|
| `run` | `command`, `platform`, `jobs`, `memory_budget_mib`, `cache`, `tool` | First event. |
| `graph` | `actions`, `cached`, `pending`, `roots` | Graph built. |
| `start` | `id`, `key`, `producer`, `source`, `label`, `est_peak_mib` | Action begins. |
| `progress` | `id`, `fraction` (0..1 or null), `detail` | At most every 100 ms per action. |
| `log` | `id` (optional), `level` (`debug`, `info`, `warn`), `text` | Producer log line. |
| `diag` | `id`, `code`, `severity` (`warning`, `error`), `source`, `line`, `column`, `message`, `hint` | Diagnostic. |
| `done` | `id`, `status` (`ok`, `failed`, `cancelled`), `wall_ms`, `cpu_ms`, `peak_rss_mib`, `peak_rss_source`, `products` (hashes), `cached` (bool) | Action ends. |
| `summary` | `ok`, `failed`, `cancelled`, `cached`, `wall_ms`, `log` | Last event before exit. |

Example:

```json
{"v":1,"t":0,"ev":"run","command":"build","platform":"host","jobs":8,"memory_budget_mib":8192,"cache":"/Users/me/Library/Caches/vkr_bakery","tool":"6b523338"}
{"v":1,"t":41,"ev":"graph","actions":412,"cached":388,"pending":24,"roots":3}
{"v":1,"t":42,"ev":"start","id":17,"key":"9f3c…","producer":"texture","source":"textures/curtain_fabric_Normal.png","label":"UASTC faster 4096x4096 normal-rg","est_peak_mib":1400}
{"v":1,"t":2410,"ev":"progress","id":17,"fraction":0.42,"detail":"mip 2/13"}
{"v":1,"t":6012,"ev":"done","id":17,"status":"ok","wall_ms":5970,"cpu_ms":30240,"peak_rss_mib":1210,"peak_rss_source":"allocator","products":["a71e…"],"cached":false}
{"v":1,"t":6100,"ev":"diag","id":18,"code":"VKR-SHD-0102","severity":"error","source":"renderer/src/shaders/vulkan/slang/world/deferred.slang","line":412,"column":9,"message":"undeclared identifier 'shadowTerm'","hint":"shared/shadow_kernel.slangh renamed the helper; see ADR-044 row 'Shadow receiver'"}
{"v":1,"t":61200,"ev":"summary","ok":23,"failed":1,"cancelled":0,"cached":388,"wall_ms":61200,"log":".vkreditor/logs/bakery-2026-09-27T10-12-03Z.jsonl"}
```

The terminal renderer, when stdout is a TTY and `--json` is absent, draws a
live table of running actions with elapsed time and progress, streams
diagnostics as they arrive and ends with the summary and the log path. Without
a TTY it prints one line per `start`, `diag` and `done`. The editor reads the
same stream from a pipe. `explain` reads the persisted stream and the action
index; it never recomputes.

Peak memory comes from `getrusage` on POSIX and `GetProcessMemoryInfo` on
Windows for worker processes, and from the allocator's high-water mark for
in-process actions; `done` states which source produced the number in
`peak_rss_source`.

## 7. Diagnostics

Codes are `VKR-<AREA>-<NNNN>` with areas `CLI`, `REC` (recipe), `IDX`
(index), `CACHE`, `SCHED`, `TEX`, `MESH`, `FONT`, `ANIM`, `COLL`, `TABLE`,
`BAKE`, `PROBE`, `SHD`, `SCRIPT`, `PROJ`, `BUNDLE`. A code has a fixed
severity and a one-line description in `vkr_bakery_diag.c`; `help diag <code>`
prints it. Each diagnostic names the offending source, line and column when a
parser knows them, and a `hint` when a known fix exists. Free-text producer
output is a `log` event, never a `diag`.

Fallible internal errors keep their existing typed error enums; the producer
maps them to codes at the boundary. A producer that crashes inside an isolated
worker yields `VKR-SCHED-0001` with the signal or exit code and the last 4 KiB
of its stderr.

## 8. Cache, index and scheduler

### 8.1 Layout

```
<cache>/
  version                    "1"
  cas/<h[0:2]>/<h>           product bytes, read-only after publish
  actions/<k[0:2]>/<k>.json  result record (see below)
  index/<workspace-id>.bin   source index: path, size, mtime_ns, file identity, hash
  lock                       cross-process lock (VkrPlatformProcessLock)
```

Action result record:

```json
{"v":1,"key":"9f3c…","producer":"texture","producer_version":3,"tool":"6b523338",
 "recipe":{"class":"normal-rg","shape":"2d","tier":"final","uastc_level":"faster","strict":true},
 "inputs":[{"path":"textures/a.png","hash":"…"}],
 "depfile":[{"path":"…","hash":"…"}],
 "products":[{"role":"vkt","hash":"a71e…","bytes":22370432}],
 "diagnostics":[],"wall_ms":5970,"cpu_ms":30240,"peak_rss_mib":1210,
 "created":"2026-09-27T10:12:09Z","last_used":"2026-09-27T10:12:09Z"}
```

Publication is atomic: write to a sibling temporary, `fsync`, rename, as
`tools/vkr_atomic_file.h` does today. A product is verified by rehashing on
first use per run when its size or mtime changed; otherwise the index entry is
trusted. Copying a product into a workspace uses `file_clone` and falls back
to a copy.

### 8.2 Source index

Hashing a 45 MB file costs 0.17 s; hashing 16 GB does not. The index stores
size, `mtime_ns` and the platform file identity per path and reuses the stored
hash when all three match. A `build` rescans the root set's directories, not
the whole workspace; the daemon updates entries from watcher events. SHA-256
remains the content identity because published fingerprints and ADR-069
inventories already use it.

### 8.3 Scheduler

Actions run on the foundation job system (`vkr_job_system.h`) with a pool of
`--jobs` workers. Each producer declares `estimate_peak_mib(recipe, inputs)`;
the scheduler admits an action only while the sum of running estimates stays
under `--memory-budget`, and an action that declares `exclusive_cores` (the
UASTC encoder with `basis_threads=auto`) runs alone while small actions fill
remaining budget. Ready actions are ordered by requester priority
(`interactive`, `build`, `background`), then by critical-path length, then by
declared cost. Cancellation sets one atomic flag that producers poll between
units of work and that kills worker processes through the existing
process-tree termination; a cancelled action publishes nothing.

In-process execution is the default. A producer marks itself `isolated` when
it runs third-party decoders that have crashed on hostile input; such actions
run in `vkr_bakery worker` with the action request on a pipe and the event
stream back on stdout. The project producer (section 9.10) is never isolated
because it owns transactions.

### 8.4 Garbage collection

`gc` removes action records whose `last_used` is older than the threshold
(default 30 days) and then any CAS entry no record references. It runs under
the cross-process lock and never touches a workspace. Workspace cleanup rules
in ADR-069 are unchanged.

## 9. Producers

Each producer implements:

```c
typedef struct VkrBakeryProducer {
  const char *id;                 /* "texture" */
  uint32_t version;               /* bump when output bytes may change */
  bool8_t isolated;
  bool8_t exclusive_cores;
  bool8_t (*validate_recipe)(const VkrJson *recipe, VkrBakeryDiagSink *diag);
  bool8_t (*declare_inputs)(const VkrBakeryRequest *req, VkrBakeryInputList *out);
  uint64_t (*estimate_peak_mib)(const VkrBakeryRequest *req);
  bool8_t (*run)(VkrBakeryContext *ctx, const VkrBakeryRequest *req,
                 VkrBakeryResult *out);   /* reports progress, depfile, products */
} VkrBakeryProducer;
```

The table below fixes each producer's identity, recipe and products. Recipe
fields not listed are rejected with `VKR-REC-0001`.

| id | Library | Recipe fields | Products | Notes |
|---|---|---|---|---|
| `texture` | `vkr_vkt_cooking` | `class` (`normal-rg`, `data-mask`, `color-srgb`, `color-linear`), `shape` (`2d`, `cube`, `array`), `layers[]`, `uastc_level`, `tier` (`preview`, `final`), `strict` | `vkt` | `exclusive_cores` when `tier=final`. `preview` uses `fastest` and a mip floor; it is a distinct key, never substituted for `final` in a bundle. |
| `mesh` | `vkr_asset_cooking` | `light_ranges{}`, `source_patches`, `bundle_root`, `import_id` | `vkb`, derived textures as separate `texture` actions | Derived texture requests become child actions instead of the cooker's private `cache/generated`. |
| `animation` | `vkr_asset_cooking` | `clips[]` | `vka` | |
| `collision` | `vkr_collision_cooking` | `kind` (`hull`, `mesh`), `node` | `vkc` | |
| `font` | `vkr_asset_cooking` font encoder | the `.fontcfg` contents | `vkfa` | |
| `table` | table generators | `table` (`dfg`, `sheen`, `anisotropy`) | `.inc` | Writes into `renderer/src`; requires a renderer rebuild, stated in `done`. |
| `diffuse_volume` | `vkr_bake_core` | `grid`, `face_size`, `samples`, `max_depth`, `seed`, `photons` | `vkdv`, manifest | Per-cell tasks scheduled on the job system. The Python wrapper's validation and publication rules move into the producer. |
| `reflection_probe` | harness capture in process | `position`, `size`, `scene` | `vkt` | Six faces in one renderer session; see section 13, phase 2. |
| `shader` | slangc, Metal toolchain | section 9.7 | `spv`, `msl`, `metallib`, `shader_manifest` | |
| `script` | platform C compiler | section 9.8 | `dylib`/`dll`, `o`, depfile | |
| `project` | today's job script logic | request document | staged bundles, inventory revision, scene document | Section 9.10. |

### 9.7 Shader producer

The shader producer owns everything `renderer/CMakeLists.txt` does with
`slangc` and the MSL concatenation today, adds a catalog the runtime loads,
and makes single-entry, single-backend and single-file compiles possible.

**Sources.** A shader library recipe lives beside each backend root:
`renderer/src/shaders/vulkan/slang/library.recipe.json` and
`renderer/src/shaders/metal/library.recipe.json`. The Vulkan recipe lists the
entries in the table `vkr_add_vulkan_packet_shader` holds today, each with
`name` (`packet.world.vert`), `entry` (`world_vertex`), `stage` and optional
`defines{}`; the profile string and optimization flags are recipe fields with
the current values as defaults. The Metal recipe lists the ordered
concatenation inputs (the current `VKR_METAL_PACKET_MSL_SOURCES` order, whose
comments explain why order matters), the Slang support library, the MSL
language version and the archive name. A change to either list is a recipe
change, not a CMake change.

**Actions.** One action per Vulkan entry: `slangc -target spirv -profile …
-entry <entry> -stage <stage> -depfile <tmp> -o <tmp.spv> library.slang`.
The depfile becomes the action's discovered inputs, so editing one shared
`.slangh` reruns only the entries that include it. For Metal: one action
compiles `metal/slang/library.slang` to MSL with `slangc -target metal
-line-directive-mode none -depfile`, one action concatenates the recipe's
ordered inputs into `library.metal` with the same separator rules as
`cmake/vkr_concat_shader_sources.cmake`, and one action per library runs
`xcrun -sdk macosx metal -c -MMD -dependency-file <tmp.d> -o <tmp.air>` followed
by `xcrun -sdk macosx metallib -o <tmp.metallib> <tmp.air>` to produce
`library.metallib` and `library.slang.metallib`. The `-dependency-file` output
is the action's depfile, so a `.metalh` edit reruns only the library that
includes it. The Metal language version and optimization flags are recipe
fields whose defaults reproduce what `MTL4Compiler` applies at runtime today;
the ADR that ships phase 1 records the chosen flags. The toolchain is probed
once at graph time: on a macOS host without it the metallib actions are
skipped with `VKR-SHD-0001` at `warning` severity, the manifest records
`metallib: null` and the runtime uses the source path; on this host the probe
succeeds and the actions are mandatory. Every shader action is in-process except the compilers
themselves, which are child processes with captured stderr parsed into `diag`
events (slangc and the Metal compiler both print `file:line:col: severity:
message`).

**Manifest.** The producer publishes `shader_manifest.json` per backend:

```json
{"v":1,"backend":"vulkan","tool":"slangc 2026.13.1","profile":"spirv_1_6+…",
 "entries":[{"name":"packet.world.vert","entry":"world_vertex","stage":"vertex","hash":"…","bytes":18324}]}
{"v":1,"backend":"metal","msl_version":"4.0",
 "libraries":[{"name":"library","source_hash":"…","metallib":"…|null"},
              {"name":"library.slang","source_hash":"…","metallib":null}],
 "archive":"vkr_application.mtlarchive"}
```

**Metal 4 pipeline archive.** The `MTL4PipelineDataSetSerializer` capture
that the renderer writes today stays a runtime concern in phase 1; phase 4
adds a `harvest` step to `bundle` that runs the bundled Bistro once per target
device class with capture enabled and stores the resulting `.mtlarchive` beside
the metallib, so a shipped game starts without pipeline compilation on a known
device and falls back to runtime compilation on an unknown one.

**Runtime loading.** `renderer/src/vkr_shader_catalog.c` opens a manifest and
resolves a name to bytes through the virtual filesystem (section 11) or, before
phase 4, through a directory. The Vulkan pipeline code replaces every
`VKR_VULKAN_<NAME>_SPV` define with `vkr_shader_catalog_spirv(catalog,
"packet.world.vert")`; the `VKR_VULKAN_REFLECTED_ROOT` validation of ADR-044
runs on the bytes it returns, unchanged. The Metal setup code prefers
`newLibraryWithData` on a present `metallib` and falls back to the existing
`MTL4Compiler` source path; `vkr_metal_packet_create_library` keeps its
diagnostic retry. The catalog path comes from one runtime setting
(`VkrRuntimeConfig.shader_catalog`), defaulting to `<build>/shaders/<backend>/`
for repository builds, so the renderer no longer embeds absolute source paths.
Behavior must be byte-identical: the SPIR-V produced by the producer for the
current recipe defaults equals the CMake output, verified with `cmp` per entry
before the CMake rules are deleted.

**On demand.** `vkr_bakery shaders --backend vulkan --entry packet.world.frag`
compiles one entry; `--watch` keeps the process alive, rebuilds affected
entries on source changes and publishes a new manifest revision atomically.
The editor, in phase 3, subscribes to manifest revisions from the daemon and
asks the renderer to recreate the pipelines whose entries changed at a frame
boundary after GPU completion; that is the hot reload `ARCHITECTURE.md`
records as absent, and it is delivered only when the renderer's pipeline
recreation path exists and is proven with a capture comparison. Until then,
the editor's Shaders tab shows compile status and diagnostics and the user
restarts.

**CMake.** Repository build wrappers must still compile shaders. Phase 1
replaces the 102 custom commands and the concatenation with one custom target
that runs `vkr_bakery shaders --backend <configured> --out
${CMAKE_BINARY_DIR}/shaders` after `vkr_bakery` itself builds; the renderer
target depends on it. Incrementality then comes from the action cache rather
than from CMake's per-file dependency scanning, and a no-op rebuild must finish
in the time it takes to hash the shader tree.

**Bundling.** Phase 4 copies each platform's manifest, SPIR-V modules,
`metallib` files and the harvested MTL4 archive into the `.vkpak`. A
Vulkan pipeline cache blob is not shipped; it is device-specific and stays a
runtime cache.

**Permutations.** The current libraries have no define-driven permutations; the
recipe's `defines{}` field exists so a future permutation set becomes new
entries with distinct names, never a runtime compile.

### 9.8 Script producer

Scripts are sources with recipes. `script.recipe.json` names `language`
(`c` now; `js`, `ts` reserved), `sources[]`, `include_roots[]`, `defines{}`
and `standard`. The C producer runs the platform compiler (`clang` on macOS,
`cl` or `clang-cl` on Windows) once per translation unit with a depfile
(`-MD -MF`), then links a shared library per script module for editor hot
reload and archives objects for static linking at bundle time. Compiler
diagnostics map to `VKR-SCRIPT-*` with file, line and column. The runtime side
(loading, ABI, lifecycle) is owned by the entity behavior proposal; this
producer only guarantees compiled, cached, bundled outputs. A JavaScript or
TypeScript producer later transpiles and bundles into bytecode for an embedded
runtime and is another entry in this table.

### 9.10 Project producer

The `project` producer ports `tools/editor_project_jobs.py` operation by
operation: `create_project`, `create_scene`, `import_project_assets`,
`import_assets`, `reimport_asset`, `rebuild_asset`, `rename_asset`,
`delete_asset`, `add_entities`, `bake_scene`, `delete_scene`,
`delete_project`. Each operation keeps its request and result document
versions, staging rules, inventory revision format, atomic publication and
cleanup behavior from ADR-069. Inside an operation, cooking calls other
producers through the graph, so an import of a Bistro model becomes one mesh
action plus one texture action per distinct image, all scheduled in parallel
under the memory budget.

Acceptance is byte identity: for the tracked fixtures used by
`tools/checks/check_editor_project_jobs.py`, `check_managed_mesh_import.py`,
`check_editor_scene_publication.py`, `check_editor_animation_assets.py` and
`check_editor_workspace_cleanup.py`, the C producer must publish identical
artifacts and documents (ignoring timestamps and identifiers the scripts
already normalize). The Python script is deleted in the same change that
switches the editor to the producer, together with `VKR_EDITOR_PYTHON_PATH`.

## 10. Editor integration

**Phase 1.** `editor_bakery.c` drops the fixed recipe table and tool-path
defines. It locates `vkr_bakery` beside the editor executable or through a
project setting, spawns it with `--json`, and a reader on the worker thread
parses events into a bounded ring of action rows (`id`, producer, source,
label, status, progress, elapsed, diagnostics). The jobs view lists rows with
per-row progress and elapsed time; the output view shows `log` and `diag`
events with severity filters and a copy button; a `diag` row with a source
opens it in the Content browser. Cancellation sends the existing process-tree
termination. The Console receives `diag` and `warn`/`error` `log` events
through the structured logger so they appear beside runtime logs. Project
jobs in `editor_projects.c` read `progress` events instead of the sidecar file
and show real fractions.

**Shaders tab.** A new Bakery tab lists both backends' manifests with per-entry
status, last compile time and diagnostics, a "Compile changed" button
(`shaders --backend <current>`) and a "Compile all" button.

**Phase 3.** `vkr_bakery serve <project>` runs one process per open project
with a file watcher (FSEvents, `ReadDirectoryChangesW`) feeding the index, a
warm graph and a local socket (`<workspace>/.vkreditor/bakery.sock`, named
pipe on Windows) carrying requests (`{"req":"build","roots":[…],"priority":
"interactive"}`) and the same event stream back. The editor starts it on
project open, restarts it on crash with a Console warning, and stops it on
close. The Content browser shows per-asset build state (`fresh`, `stale`,
`building`, `failed`) from `status` results, and editing a source triggers a
`background` rebuild whose products the runtime picks up through the existing
reload paths.

## 11. Bundle and runtime virtual filesystem

`vkr_bakery bundle <project> --platform <p> --config <c> --out <dir>` builds
the project's root set for the platform with `tier=final`, then writes:

```
<out>/
  <game executable>            copied platform runtime (vkr_runtime host)
  content/<project>.vkpak
  content/shaders.vkpak
  bundle.json                  tool identity, platform, config, action keys of every product
```

`.vkpak` format, version 1, little-endian:

| Section | Contents |
|---|---|
| Header | magic `VKPK`, version, catalog offset and size, chunk table offset and size, total size, SHA-256 of the catalog and chunk table. |
| Chunk table | `(hash, offset, size, alignment)` sorted by hash; offsets are 64-byte aligned by default and 4 KiB aligned for mappable resources. |
| Catalog | portable asset identities (`scope/id/role` from ADR-069, shader names from 9.7) to chunk hash plus loader kind and version; UTF-8 strings, `/` separators, no host paths. |
| Chunks | product bytes, stored once per hash. |

`runtime/src/vfs/` mounts a loose directory (editor, repository builds) or
one or more `.vkpak` archives (shipped game) and resolves an identity to a
read-only byte view, memory-mapped when the platform allows. Loaders that open
paths today (`mesh_loader`, `cooked_font_loader`, texture system, scene
loader, shader catalog) call the virtual filesystem instead; their format
validation is unchanged. Two mounts of the same identity resolve to the first
mount in order, which is how a patch archive overrides a base archive.
Deduplication and patching follow from chunk addressing.

## 12. Constraints carried from the repository contract

- Preserve every published artifact format and version, ADR-069 workspace
  layout, ADR-070 path rules and ADR-044 native validation.
- One producer per artifact kind, one cache, one event protocol; no second
  orchestration layer.
- Python remains only for CPU check scripts and analysis, never on the
  editor's or runtime's path.
- Legacy repository recipes (Bistro re-cook, tables, room bake, harness
  fixtures) become `vkr_bakery cook` invocations with checked-in recipes; the
  shell wrappers under `tools/` are deleted when their recipes land.
- Release with graphics validation unset for every measurement; one Metal
  validation process at a time; no broad shader-validation capture suite.

## 13. Phases, deliverables and acceptance

Each phase is independently shippable and ends with an ADR update through
`vkr-docs`. Baselines are recorded once and reused.

### Phases 0 to 5: shipped

Recorded in [ADR-077](../adr/077-asset-build-system.md) with their evidence and
deviations. Open acceptance items: native Vulkan captures through the catalog
path, the daemon, a Vulkan bundle and script modules on Windows, an editor
frame-time measurement with the daemon running, and shader hot reload once the
renderer can recreate pipelines.

## 14. Decisions taken

The user accepted these on 2026-09-27; they are defaults for implementation.

| Decision | Choice | Rationale |
|---|---|---|
| Binary layout | One multi-call `vkr_bakery` with a `worker` mode | Removes spawn cost and the flag zoo; the CMake libraries already expose the producers |
| Python orchestration | Port to the `project` producer in phase 2 behind the existing check scripts, then delete | Interpreter dependency and serial execution are the two structural faults |
| Cache | One per-machine cache, action key from producer version, recipe, inputs, platform and tool identity, SHA-256 identity | Shares work across workspaces; matches published fingerprints |
| Editor transport | Piped child with `--json` in phase 1, daemon in phase 3 | Ships value early; the protocol is the same |
| Texture format | Keep UASTC per ADR-012, add `preview`/`final` tiers | Native BC7/ASTC reopens ADR-012 and needs measured load-time evidence |
| Shaders | Producer-owned recipes and manifests, runtime catalog, CMake reduced to one target, `metallib` when the toolchain exists | On-demand compiles and bundling need shaders to be data, not build-tree paths |
| Hot shader reload | Delivered only with a proven pipeline recreation path | Recorded as absent today; a partial reload risks GPU-completion invariants |

Ask before: changing any artifact format version, shipping a different texture
format, altering the ADR-069 workspace layout, or spending more than one
Metal validation run per phase.

## 15. Evidence, documentation and cleanup at ship

- Update `docs/ARCHITECTURE.md` build and editor sections, `docs/CONTEXT.md`
  (`Bakery`, `Cooked asset`, `Derived texture cache` rows; add `Action`,
  `Product`, `Recipe`, `Catalog`), the relevant ADRs (044 shader loading, 069
  jobs, new ADR for the build system) and `docs/INDEX.md` together.
- Update `.codex/skills/vkr-harness` and `vkr-shaders` where they name cooker
  wrappers or CMake shader rules, and keep `.claude/skills/` identical.
- Remove replaced executables, wrappers and scripts in the phase that replaces
  them, never earlier. Phases 0 to 2 removed the cooker executables, wrapper
  scripts, CMake shader rules and Python jobs, bake and preview scripts.

## Code baseline

- [tools/bakery](../../tools/bakery) and its
  [project runner](../../tools/bakery/project/vkr_project_main.c);
  [editor_bakery.c](../../editor/src/editor_bakery.c),
  [editor_content.c](../../editor/src/editor_content.c).
- [Shader catalog](../../renderer/src/vkr_shader_catalog.c),
  [vkr_metal_packet_setup.inc](../../renderer/src/metal/internal/vkr_metal_packet_setup.inc).
- Foundation: [vkr_job_system.h](../../lib/src/core/vkr_job_system.h),
  [filesystem.h](../../lib/src/filesystem/filesystem.h),
  [vkr_platform.h](../../lib/src/platform/vkr_platform.h).
- Decisions in force: [ADR-012](../adr/012-texture-compression-pipeline.md),
  [ADR-044](../adr/044-shader-cross-backend-contract.md),
  [ADR-069](../adr/069-editor-projects-and-workspaces.md),
  [ADR-070](../adr/070-portable-path-boundaries.md),
  [ADR-077](../adr/077-asset-build-system.md); related proposal
  [entity behavior system](entity-behavior-system.md).
