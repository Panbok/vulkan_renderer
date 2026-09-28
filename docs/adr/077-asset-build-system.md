---
status: partial
updated: 2026-09-28
authority: adr
---

# ADR-077: One asset and shader build system

## Status

Accepted (partial). Every phase of the
[asset build system proposal](../proposals/asset-build-system.md) is
implemented on macOS. The daemon has no Windows transport, the Windows script
and bundle paths are unverified, bundles are built from repository scenes
rather than managed projects, and script modules have no runtime loader yet.

## Context

Asset generation was spread across ten cooker executables, shell and batch
wrappers, CMake custom commands for Slang and Metal shaders, a 3,000-line Python
project job runner and three Python bake/preview scripts. Nothing shared a cache
key, progress protocol or diagnostic vocabulary; shader compilation ran only at
CMake build time; the editor needed a Python interpreter; and Metal compiled
every shader library from source at startup.

## Decision

[`vkr_bakery`](../../tools/bakery/vkr_bakery_main.c) is the single build program
for cooked assets, lookup tables, shaders, managed project transactions and
scene bakes. It is always built, because the renderer's shader catalog comes
from it.

- **Producers and actions.** [Producers](../../tools/bakery/vkr_bakery_registry.c)
  turn a source plus recipe into products: `texture`, `mesh`, `animation`,
  `collision`, `font`, `table`, and the shader producers. An action's key hashes
  the producer identity, version, platform, recipe and input hashes, then the
  inputs its depfile discovered. Producer identities are generated from the
  source files that can change their bytes
  ([identity rules](../../cmake/vkr_bakery_identity.cmake)).
- **Cache.** A content-addressed store, action records and a path-hash
  [index](../../tools/bakery/vkr_bakery_index.c) live in one per-user cache
  (`--cache`, `$VKR_BAKERY_CACHE`, or the platform cache directory), shared by
  checkouts and workspaces. Products publish by clone-or-copy and atomic rename;
  [`gc`](../../tools/bakery/vkr_bakery_cache.c) removes unused entries.
- **Scheduler.** [Workers](../../tools/bakery/vkr_bakery_graph.c) run ready
  actions in priority order under `--jobs` and a memory budget from each
  producer's peak estimate; final-tier UASTC encodes share two all-core
  slots, while preview-tier and ASTC encodes, which stay near one core, run
  like any other action.
  SIGINT/SIGTERM cancel pending actions and terminate children without
  publishing partial products.
- **Isolation.** Cookers are static libraries linked into `vkr_bakery` and run
  as `vkr_bakery tool <name>` child processes. This deviates from the
  proposal's in-process default: the cookers keep process-global state, and the
  measured spawn cost is negligible against encode time.
- **Protocol.** Every command emits version 1 JSON event lines (`--json`) or
  human lines, with `VKR-<AREA>-NNNN` diagnostics and exit codes 0 success,
  1 failure, 2 usage, 3 cancelled and 4 environment. `project` keeps its
  request contract's codes: 1 failed, 2 cancelled, with the status in its
  result document.
- **Shaders.** The `shaders` command compiles each Vulkan entry to SPIR-V and
  the Metal libraries to MSL and a `.metallib` from the checked-in
  [Vulkan](../../renderer/src/shaders/vulkan/slang/library.recipe.json) and
  [Metal](../../renderer/src/shaders/metal/library.recipe.json) recipes, and
  writes a per-backend manifest. The `vkr_shaders` CMake target runs it into
  `<build>/shaders`. The renderer resolves files through the
  [shader catalog](../../renderer/src/vkr_shader_catalog.c): an explicit root,
  `$VKR_SHADER_CATALOG`, `<executable dir>/shaders`, then the build tree. Metal
  loads a `.metallib` only when the manifest's source hash matches the MSL
  beside it and otherwise compiles the source; `VKR_METAL_COMPILE_SOURCE=1`
  forces source compilation.
- **Project transactions.** `vkr_bakery project --request --result`
  ([runner](../../tools/bakery/project/vkr_project_main.c)) owns every managed
  project job with version 1 request/result documents: scene creation,
  preparation and bakes, project and scene asset imports, rebuild, reimport,
  rename, deletion, scene inspection, deletions after membership removal,
  workspace cleanup (`collect_garbage`) and the bake input preview
  (`effective_bake_runtime`). Material textures pack through cached `texture`
  actions; a repository `<source>.vkt` seeds its action. Every digest goes
  through the cache's path index, shared with the texture graph, so a file
  whose size, modification time and identity are unchanged is not read
  again; publishing a staged revision moves its entries to the published
  paths, and a snapshot copy is the hashed bytes when the source's stat is
  unchanged across the copy. The mesh cooker converts each spec-gloss
  material on the worker that writes its material file and packs the
  converted base color and metallic-roughness from memory, as a cutout
  variant, as the roughness of a paired bake or as a plain texture, into one
  pixel-named directory of the shared generated root, so importing a model
  again finds them instead of encoding them; the runner packs only textures
  still named by source image. Repository cooks, whose runtime reads
  materials' images as sources, keep writing converted PNGs. Material files,
  cutout variants and paired normal/roughness bakes run on eight workers at
  the preview tier and with ASTC and on three at the final UASTC tier,
  publishing results in material order. The cooker hashes each bundle
  dependency once, in parallel, with SHA-256: the digest's first 64 bits name
  the bundle copy and the cooked mesh's dependency table records the digest
  without reading the copy again.
  SHA-256 uses the ARMv8 instructions on Apple silicon.
- **Texture tiers and encodings.** A request's `texture_tier` (`final` by
  default, `preview`, `deferred`) and `texture_encoding` (`uastc`, `astc` or
  `astc-fast`; by default ASTC on Apple silicon and UASTC elsewhere, per
  [ADR-012](012-texture-compression-pipeline.md)) select how material textures
  and the mesh cooker's derived textures are built (`--texture-tier`,
  `--texture-encoding`). `texture_encode_speed` `fast` turns `astc` into
  `astc-fast`, Apple's system encoder, as Unreal's editor encodes new
  textures at its `Fast` speed and cooks at `Final`; UASTC has no fast
  encoder and ignores it. The editor sends `fast` on every project job, so
  textures only it shows take the faster encoder, while explicit and
  command-line requests keep astcenc. A bundle of a managed project, which
  does not exist yet, would rebuild `astc-fast` textures at the final speed. Preview keeps a 1,024-pixel mip floor: levels larger
  than that are not stored (`--max-extent`, part of the settings identity),
  and UASTC previews use its fastest level. `deferred` builds no texture: the
  cooker keeps material factors, resolves and copies no image, and a scene
  opens at once. Every tier and encoding has its own names (`-preview`,
  `-astc`, `.astc.preview`, `-astc-fast`, ...) and keys, so one never
  replaces another. A
  mesh record imported at the preview or deferred tier carries
  `"texture_tier"`, and every scene result reports those records as
  `preview_assets`. `finalize_textures` rebuilds them at the final tier as one
  scene publication; entities keep their asset identities.
  `import_project_assets` reports its own such records, and
  `finalize_project_assets` rebuilds the project's at the final tier and
  returns the new inventory, which the editor publishes. The editor sends
  `deferred` for scene jobs and Content imports and runs the matching
  finalize job in the background, Content first, whenever a result or the
  saved inventory holds such assets: any other project job cancels it and it
  retries when the editor is idle. A project inventory is adopted only if it
  is unchanged since the job started.
- **Progressive finalization.** A finalize request may name a `ready_log`
  and a `material_priority`. The mesh cooker (`--ready-log`,
  `--material-priority`) appends one JSON line per material whose textures
  are all packed at absolute paths as soon as its worker writes the file:
  its name, its file and its text, whose references stay valid after the
  staged revision moves. Priority materials start first; results are still
  gathered in material order. After publishing, the job appends the rest,
  whose textures its texture step packed, from their published files. The
  editor requests the open scene's materials ordered by the share of the
  Scene view their submeshes' projected bounds cover (occlusion is not
  considered), reads the log ten times a second and hands each record to
  `vkr_material_loader_replace_live`, which replaces the live material of
  that name through `vkr_material_system_replace`
  ([material system](../../runtime/src/renderer/systems/vkr_material_system.c)):
  the textures stream as a group and the definition and every texture
  publish in one republication once all have loaded, so no frame mixes old
  factors with new textures; the material's earlier resident textures are
  released after the new row publishes. Replacements never touch the scene
  document, undo history or dirty state. A finished job whose records all
  applied to the scene generation it started with adopts the published
  manifest fingerprint without reopening; otherwise a clean open scene that
  names a finalized asset reopens as before, and anything else takes effect
  when the scene next opens. Materials applied before a cancellation or
  failure stay: their textures are final and content-addressed in the
  generated root.
  Indexing a bundle writes `.textures-packed` into the revision once every
  material names packed textures; preparing a scene reads the materials of
  revisions without it only.
- **Bakes and previews.** `bake diffuse` and `bake probe`
  ([bakes](../../tools/bakery/vkr_bakery_bake.c)) keep the former scripts'
  options, provenance sidecars, evidence directories, `--check` and exit
  status 3 for a volume without closed-room cells. `preview material` renders a
  managed material thumbnail in an isolated harness run; `preview prune`
  bounds the thumbnail cache. The diffuse baker traces probes on worker threads
  (`--threads`, zero for every hardware thread).
- **Editor.** Bakery recipes, project jobs, scene bakes and material previews
  launch only `vkr_bakery`; the editor needs no Python interpreter. Recipes
  stream events into per-action progress and Console lines.
- **Daemon.** `vkr_bakery serve --root <dir> [--socket <path>] [--idle-exit
  <s>]` ([daemon](../../tools/bakery/vkr_bakery_serve.c)) accepts version 1
  newline-delimited JSON requests on a local socket: `run` (the `cook`,
  `build`, `shaders`, `status`, `inspect`, `explain` and `gc` commands, with
  interactive requests ahead of watch rebuilds), `watch` (paths, with an
  optional command rerun after each settled change), `unwatch`, `cancel`,
  `ping` and `shutdown`. Events carry the request (`req`) or watch (`watch`)
  that caused them and end with `{"ev":"reply","exit":N}`; `done` events list
  published outputs so the daemon ignores its own writes. One daemon serves a
  socket; a stale socket is replaced; SIGINT/SIGTERM cancel the running
  command and stop the daemon. `vkr_bakery send` is the command-line client.
  FSEvents feeds the watcher on macOS; Windows reports the daemon unavailable.
- **Editor daemon.** Each editor process
  ([service](../../editor/src/editor_bakery_service.c)) starts one daemon on a
  per-process socket under the temporary directory when the first watch
  registers, reads its events without blocking, restarts it after an exit with
  a Console warning (at most five restarts a minute), re-registers watches,
  reruns a rebuild the exit interrupted, and shuts it down on close. It
  deviates from the proposal's per-project socket in the workspace: watches
  already scope work to the open project, and a per-process socket needs no
  cross-editor ownership rule. Two watches use it:
  - Shader sources recompile the catalog this editor loaded; the Console asks
    for a restart because the renderer has no pipeline hot reload.
  - The open project's directory marks a Content item Changed or Missing when
    its source or artifact changes on disk after the listing was read. An
    edited source of an asset in the open scene queues that asset's Rebuild
    job without a user request; with unsaved scene edits the item stays
    Changed and the Console asks for a save and Rebuild. The Content browser
    has no per-action `status` state: a project job owns publication.
- **Content root and archives.** Relative asset paths resolve against one
  content root ([mounts](../../lib/src/filesystem/vkr_vfs.h)): the repository
  by default, or a bundle's `content/` directory. `.vkpak` archives mounted
  over the root serve an identity, its root-relative path, before the
  directory does; the first archive holding it wins. Every read-only
  `file_open`, `file_fopen`, `file_exists` and `file_stats` below the root
  consults the mounts, so loaders keep their paths and validation; archive
  entries read from mapped bytes. Mounting happens once at startup:
  `$VKR_CONTENT` or a `bundle.json` beside the executable mounts a bundle, and
  `$VKR_CONTENT_PACKS` overlays archives on the repository root.
  `$VKR_VFS_RECORD` appends every content read to a file. With a bundle
  mounted, the render graph also comes from content, and the texture
  transcode cache defaults to the content root. Transcode cache entries are
  named by the `.vkt` content hash and target format, so a texture reached
  through several paths (projects, repeated imports, bundles) transcodes and
  stores once; before, each path added its own entry, about 3.4 GiB per new
  Bistro import.
- **Bundles.** `vkr_bakery bundle <recipe> --out <dir> [--app <executable>]
  [--shaders <catalog>]` ([bundle](../../tools/bakery/vkr_bakery_bundle.c))
  writes `content/<name>.vkpak`, copies the runtime and shader catalog beside
  it, and writes `bundle.json` (scene, platform, configuration, archives and
  each identity's SHA-256). A recipe
  ([Bistro](../../assets/bundles/bistro.bundle.json)) names a scene and
  runtime resources; the closure follows the loaders: scene documents name
  meshes, materials and cubemap faces, a mesh names its materials, a material
  or font configuration names textures and atlases, and a texture reference
  resolves to its `.vkt` sibling while cubemap faces keep their images. The
  archive (version 1, little-endian) holds a 128-byte header with the catalog
  and chunk table ranges and a SHA-256 over both, chunks stored once per
  content hash (64-byte aligned, 4 KiB for meshes, textures and volumes), a
  hash-sorted chunk table and an identity-sorted catalog of relative
  identities with a loader kind. `bundle` validates the written archive with
  the runtime's reader and rehashes every chunk. The shader catalog stays a
  directory beside the executable, which the catalog lookup already resolves,
  rather than a second archive.
- **Scripts.** A C script module is `<module>.script.json` (`language` `c`,
  `sources`, `include_roots`, `defines`, `standard`; paths relative to it),
  built by `cook` ([producers](../../tools/bakery/vkr_bakery_script.c)). Each
  translation unit is one cached `script_object` action whose key holds the
  compiler's version line and whose depfile adds the headers it read; one
  `script_library` action links the hot-reload library (`lib<module>.dylib`)
  and a deterministic static archive (`lib<module>.a`) beside the description
  or under `--out`. Compiler errors and warnings become `VKR-SCRIPT-0100` and
  `0101` with file, line and column; a missing compiler is `VKR-SCRIPT-0102`.
  A bundle recipe's `scripts` ship their archives under `<out>/scripts/` and
  in `bundle.json`. Loading, the script ABI and linking archives into the
  runtime belong to the
  [entity behavior proposal](../proposals/entity-behavior-system.md); these
  producers only compile, cache, diagnose and publish. Windows uses
  `clang-cl`, `lld-link` and `llvm-lib`.

## Consequences

- One cache key serves command-line cooks, repository manifests
  (`vkr_bakery build assets/bakery.json`) and editor imports, so unchanged
  inputs never re-encode.
- Shader edits rebuild through `vkr_bakery shaders`; every Vulkan entry includes
  the single `library.slang`, so a shared header edit recompiles every entry.
- Rollback checks inject failures with `VKR_BAKERY_FAULT_STAGE=<progress
  label>` and `VKR_BAKERY_FAULT_CANCEL_AFTER=<document path>`; production
  requests never set them.
- Mesh-cooker derived textures still use the workspace `cache/generated`
  directory rather than child `texture` actions. Repository cooks already cache
  them as mesh side products and managed imports reuse the content-addressed
  directory; child actions would need dynamic graph expansion and a second
  mesh pass.
- A Rebuild queued by a source edit republishes the scene and reloads it like
  a clicked Rebuild.
- A bundle is a directory that runs from anywhere; a patch archive mounted
  ahead of the base overrides it by identity.

## Evidence

M1, Release, 2026-09-27:

- `vkr_bakery shaders`: 108 actions cold in 18.6 s against 16.9 s for the former
  parallel CMake rules; warm 0.47 s. All 102 SPIR-V modules and both MSL
  outputs are byte-identical to the CMake outputs.
- Metal library creation: source 436 + 2,817 ms cold versus metallib 0.5 +
  2.0 ms. `bistro_windowed_snapshot` differs by 13 pixels, maximum 1/255,
  between metallib and source compilation; metallib run-to-run noise is
  7 pixels, maximum 3/255.
- Cooked products are byte-identical to direct tool runs for mesh, both texture
  packer modes, font, collision hull and mesh, animation and the DFG table.
- The project runner passes `check_editor_project_jobs.py` (including the
  diffuse bake), add-entities, animation-assets, scene publication, workspace
  cleanup, scene deletion, `check_path_contract.py` and
  `check_path_lifecycle.py` through [`project_jobs.py`](../../tools/checks/project_jobs.py),
  in Release and under ASan/UBSan.
- Diffuse probe threads on
  [the enclosed-room fixture](../../assets/scenes/fixtures/bakery_diffuse_room.scene.json):
  6.98-7.18 s with `--threads 1` versus 1.30-1.31 s on every thread, with
  identical output bytes to the former serial baker.
- Bistro reflection probe, 64² faces at (-7.5, 2.2, 9): one harness session
  (`capture_session: "single"`, [ADR-051](051-renderer-harness-and-evidence.md))
  takes 14.2 s against 83.7 s for six sessions. Five faces are byte-identical
  and -X differs in 18 binary16 components by one unit in the last place; a
  repeat is byte-identical and `--check` reports it current.
- `preview material` output equals the former Python script's
  (SHA-256 `91e2ec20…`).
- Bistro managed import through `project` (fresh workspace and cache, then a
  second import into the same project restored from the cold state before
  each run; two runs each):

  | Change set | First import | Second import |
  |---|---|---|
  | Before this work | 1,041 s | 587, 593 s |
  | Path index, ARMv8 SHA-256, shared spec-gloss directory | 718 s | 130, 129 s |
  | Plus parallel cooker work, two texture slots, 24-hour grace for unreferenced derived files | 463, 443 s | 51, 51 s |

  The first import now spends 263-280 s in the mesh cook (535 s before) and
  159-162 s in 185 texture encodes (206 s); the second, 37-42 s in the mesh
  cook (287 s), with every texture action cached. Materials and every
  referenced texture are byte-identical to the original runner's once the
  per-import namespace is normalized; the cooker's bundle and derived
  textures are byte-identical between serial and parallel builds, cold and
  warm; 40 Bistro textures encode to identical bytes with one, two or three
  slots (111, 91 and 90 s). Peak child RSS rose from 1.8 to 2.4-2.6 GiB.
  SHA-256 alone: 212 to about 1,900-2,000 MB/s, identical digests.
  Cancelling during the snapshot and after three encodes exits 2 with the
  project, scenes and staging unchanged, only completed content-verified
  products in the cache and no orphan processes.
- Texture tiers on the same host: a cold Bistro import at the preview tier
  took 114 and 127 s with the mip floor (mesh cook 72-82 s, 185 texture
  encodes 26-28 s; 205 and 225 s at UASTC fastest alone) against 443-463 s at
  the final tier; `finalize_textures` then took 417 s. The finalized scene's
  materials and textures are byte-identical to a direct final import's once
  the per-import namespace is normalized. In a scripted editor run opening
  that preview scene, the background job finished in 7 min 45 s while the
  scene stayed open, the clean scene reopened, and its inventory held no
  preview records. [`check_editor_texture_tiers.py`](../../tools/checks/check_editor_texture_tiers.py)
  covers preview naming, finalization and equality with a final import.
- Preview-tier Bistro imports on 2026-09-28, interleaved with the previous
  build on the same host (fresh state for each first import, then two second
  imports): first import 140.9 and 156.2 s before, 103.0 and 103.7 s after;
  second import 42.7-47.8 s before, 12.1-13.9 s after. Five changes: a memo
  of specular-glossiness conversions (second-import mesh cook 24 to 8 s);
  converted PNGs written by miniz at level 2, about three times
  `stb_image_write`'s speed and 20% smaller on 16 converted Bistro images with
  identical decoded pixels; per-dependency SHA-256 on worker threads, with the
  cooked mesh's source hash taken over the per-file digests rather than every
  byte a second time; bundle copies inheriting their source's content hash;
  and preparing a just-imported scene no longer rereads the materials the job
  itself pointed at packed textures (8.6 s for 254 new files). Materials and
  every referenced texture match the previous build's except the `.vkt`
  `vkr.source_hash` field, which names the PNG's bytes. Converted images
  are no longer written as PNGs (see below). Rejected: stored (uncompressed)
  PNGs cut cook CPU
  but grew the converted directory to 5.0 GiB and slowed the second import
  from 36 to 46 s.
- Core use on the same host, 2026-09-28: sampled each second, a preview
  first import kept only one or two of eight cores busy after the
  conversion phase, and its 185 encodes took 83 s of CPU in 70 s of summed
  wall time at two slots. Preview encodes now share the cores and the
  cooker runs eight paired bakes at a time at the preview tier (three at the
  final tier). Interleaved first imports: 103.8 and 132.8 s before, 93.8
  and 94.1 s after (texture step 29.7 and 39.4 s to 21.6 and 18.4 s); the
  cooker's bundle and derived files are byte-identical and materials and
  textures match. Rejected: encoding the converted base colors while the
  cook ran, from a list the cooker published after conversion, into the
  action cache. The later texture step then found 167 of 185 cached, but
  the cook slowed by about as much, and first imports were 75.5, 88.2 and
  118.8 s against 84.4-96.2 s without it. The host was swapping
  throughout, so single runs varied by up to half; only interleaved pairs
  are compared.
- Native ASTC and deferred imports on the same host, 2026-09-28, with the
  browser load gone (preview UASTC first import 70.1 s, re-import 9.3-10.2 s,
  `finalize_textures` 442.9 s and 2,620 CPU-seconds before this work). Per
  encode, 8 cores, level 0 of 8 textured Bistro images: UASTC fastest 32
  Mpx/s at 49.3 dB, UASTC faster 7.9 Mpx/s at 52.2 dB, ASTC 4x4 fast 88 Mpx/s
  at 52.9 dB. A final-tier Bistro import took 408.4 s with UASTC and 125.6 s
  with ASTC fast. Profiles then placed the cost in the paired normals: baked
  normals encode at 12.6 Mpx/s against 70 for source normal maps, and storing
  their unused alpha as one encodes 1.4 times faster and 2 dB better; skipping
  the per-texel encoding of preview levels that are not stored saved 10
  CPU-seconds with identical output. With the `fastest` preset, hashing new
  bundles and snapshot sources in parallel and the packed-texture marker, a
  deferred first import opens after 5.3-5.7 s (cook 3.2 s), `finalize_textures`
  takes 76.5 s, a deferred re-import 4.0-4.2 s and its finalization 10.2 s,
  and preparing the finalized scene 0.30 s instead of 8.48 s. A headless
  editor run opened the deferred scene, finalized it in the background (88 s
  while rendering) and reopened it. Rendered through the Bistro windowed
  snapshot camera with manual exposure 16, the ASTC scene differs from the
  UASTC one by 0.40/255 on average (PSNR 49.1 dB, 1,140 pixels above 10/255,
  largest on high-frequency edges); ASTC re-renders are bit-exact. Measured
  and rejected: one encoder thread per paired bake (unchanged time and CPU)
  and texture-bake duplication (15 of 339 encodes).
- Content imports, same host: a deferred `import_project_assets` of Bistro
  took 4.5-5.1 s. A headless editor opened a scene naming the deferred
  project mesh, started `finalize_project_assets` from the saved inventory,
  published the result (10.6 s with a warm cache) and reopened the scene
  (1.7 s). [`check_editor_texture_tiers.py`](../../tools/checks/check_editor_texture_tiers.py)
  covers the deferred Content import and its finalization.
- Build-performance round on the same host, 2026-09-28, with three read-only
  code reviews (mesh cook, project runner, texture encoding) followed by
  measurement. Bistro cold deferred import 5.4 s to 2.1-2.2 s and re-import
  4.0 to 1.8 s (warm binaries, interleaved); `finalize_textures` 76.5-84.4 s
  to 61.4-62.1 s. Contributions: cook dependencies limited to sampled images
  (deferred cook 3.4 to 1.3 s, 12.5 to 4.7 CPU-seconds; bundle dependencies
  3.3 GB to 95 MB), ranges encoded in parallel (1.26 to 0.80 s, identical
  `.vkb`), normals encoded at 39 dB with one candidate (cook 347 to 290
  CPU-seconds; other ASTC outputs byte-identical), cleanup skipping young
  derived files and index entries following published revisions (finalize
  tail 3.4 to 0.2 s). Rendered at exposure 16 the finalized scene differs
  from the previous ASTC build by PSNR 59.4 dB (32 pixels above 30/255, at a
  lamp edge) and from UASTC by 49.1 dB as before. Measured without gain:
  looser searches for metallic-roughness (preset already 140 Mpx/s) and the
  two-channel normal layout (43.6 dB but no faster).
- Import paths, same host, interleaved with warm binaries: snapshots that keep
  only sampled images (343 PNGs instead of 686 files; cook starts at
  0.42-0.61 s instead of 0.92-1.25 s) and inspection reports the cook writes
  itself, keyed by artifact digest (no inspection process), bring a cold
  deferred Bistro import to 1.6 s and a re-import to 1.4-1.5 s. A finalized
  scene from a pruned snapshot has byte-identical materials and textures.
- Fast encode speed, same host, fresh workspace and cache for each run, in
  both orders: a cold Bistro `finalize_textures` took 48.0 and 49.4 s with
  `astc-fast` (268-269 CPU-seconds; model rebuild 34.3-36.9 s, texture step
  10.1-11.1 s) against 64.9 and 66.3 s with astcenc (392-403 CPU-seconds;
  45.3-46.5 s and 17.2-17.4 s). Per class on one in six of the finalize's
  pre-encode images, see ADR-012. Rendered through the Bistro snapshot camera
  at exposure 16, the fast scene differs from the astcenc one by PSNR 49.0 dB
  (mean 0.39/255, 87 of 2.8 million pixels above 30/255, on picture frames
  and bottles rather than normal-mapped surfaces), the size of the accepted
  ASTC-against-UASTC difference. A headless editor opened a deferred Bistro
  scene, ran its background finalization with `texture_encode_speed` `fast`
  (36 s with the conversion memo warm) and reopened the scene on
  `-astc-fast` textures.
- Converted specular-glossiness images packed from memory, same host, fresh
  workspace and cache for each cold run, interleaved: a cold Bistro
  `finalize_textures` at `astc-fast` took 35.5 and 35.8 s (205 CPU-seconds)
  against 50.2 and 45.2 s (270) with the converted PNGs, the runner's
  texture step fell from 185 encodes to 11, and 1.0 GiB of PNGs is no longer
  written; a repeat import and finalization took 7.1 s against 7.9 s. Peak
  child memory rose from 2.8-2.9 to 3.2-3.4 GiB, since each worker holds its
  material's converted images until they are packed. All 254 materials and
  every texture they name match the PNG build's, except the `.vkt`
  `vkr.source_hash`, which now names the converted pixels.
  [`check_spec_gloss_memo.py`](../../tools/checks/check_spec_gloss_memo.py)
  covers memo reuse, an edited source, and a deleted packed texture that is
  converted again byte-identically.
- Progressive finalization, same host, headless editor on a fresh deferred
  Bistro scene with the default editor camera (243 of 254 materials in view),
  three runs each against a build without the ready log: the first material
  shows 0.1 s after the job starts (factor-only materials), half of those in
  view after 12.8-13.0 s, and every material with the job (40.6-43.7 s); the
  scene then settles at full quality 41.9-45.6 s after the start without a
  reopen, against 41.2-41.4 s for job (34.5-34.6 s), reopen and streaming in
  the old flow. Loading textures while the cook runs slows the job by about
  7-9 s. Median frame time while finalizing: 7.8-8.5 ms against 7.8-8.6 ms,
  95th percentile up to 58 ms against 45 ms; idle afterwards 5.9-6.8 ms. The
  standalone job with and without the priority list: 32.5 and 33.3 s against
  32.4 and 31.9 s. All 254 records match the published materials (non-texture
  lines equal, every texture reference resolving to the same bytes with the
  same query), and the published mesh's compressed geometry payload (the last
  29.8 of 30.9 MB) is byte-identical to the deferred revision's, so the
  adopted scene holds what a reopen would load. The editor's Cmd `stats` root
  reports frame times, replaced and pending materials and pending textures
  ([ADR-075](075-editor-cmd-bar-and-evaluator.md)).
  [`check_editor_texture_tiers.py`](../../tools/checks/check_editor_texture_tiers.py)
  covers the job's records for scene and Content finalization and
  [`check_spec_gloss_memo.py`](../../tools/checks/check_spec_gloss_memo.py)
  the cook's; the material replacement's single publication and release
  order are covered by `test_material_replacement_publishes_as_one`
  ([material tests](../../tests/src/material_pbr_tests.c)). Not exercised:
  cancellation, a save and closing the scene during the job in the editor
  (headless `scene.save` and reopening the open scene start no job there),
  and a pixel comparison of the live editor view, which the harness cannot
  capture.
- One hash per bundle dependency, same host: the cook read each of about
  4.1 GB of dependencies twice (FNV-1a for the bundle name, SHA-256 for the
  dependency table). With SHA-256 alone a cold Bistro finalization took
  195 CPU-seconds against 205 (wall 32.8-38.8 s against 34.7-36.2 s; this
  host's run-to-run spread is now about 5 s), a repeat finalization 5.6 s
  against 7.1 s (13.5 against 22.4 CPU-seconds), with identical materials
  and textures. Profiled, the cook's CPU is now about half Apple's encoder,
  and 13% each PNG decoding and specular-glossiness conversion. Measured
  without gain and reverted: the system encoder without its own threads,
  materials started largest first (38.0-38.9 s, more memory), digests
  recorded as the packer writes (the tail is the last materials' paired
  bakes, not hashing) and row-parallel paired mips and downsampling (35.5
  against 35.7 s mean over three interleaved pairs).
  [`check_editor_texture_tiers.py`](../../tools/checks/check_editor_texture_tiers.py)
  covers fast naming and identity, and UASTC imports unchanged by it.
- [`check_bakery_serve.py`](../../tools/checks/check_bakery_serve.py) covers
  tagged runs, one rebuild per edit without self-triggering, cancellation,
  disconnection, a second daemon, a stale socket, shutdown, idle exit and
  SIGTERM during a command.
- Editor daemon, scripted Release runs: on Bistro, touching a shared shader
  header rebuilds the catalog in the background; killing the daemon with
  SIGKILL mid-rebuild logs the exit (137), restarts it, reruns the rebuild and
  reports the result; quitting leaves no daemon, socket or log. On a
  one-texture project fixture, rewriting the texture's source runs one
  Rebuild job that publishes the new content under a new revision; with an
  unsaved scene edit, no job runs and the Console asks for a save.

- Bistro bundle: 681 entries, 2.49 GiB, written and verified in 34 s. Its
  closure equals the set of content files the loose runtime reads
  (`$VKR_VFS_RECORD`) apart from two absent optional sidecars the runtime
  probes and three font and graph sources the loose runtime reads elsewhere.
  Copied to `/tmp` and run under a sandbox profile that denies every read and
  write below the repository, the bundled app loads Bistro and exits 0,
  reading 682 identities, all from the archive.
- `bistro_windowed_snapshot` with the archive mounted over the repository
  differs from loose runs in 18 and 7 pixels (maximum 3/255); two loose runs
  differ in 17 pixels (maximum 3/255). Every scene read of the archive run is
  in the archive.
- [`check_bakery_bundle.py`](../../tools/checks/check_bakery_bundle.py) reads
  the archive with an independent parser; the filesystem suite checks archive
  validation and every read path over a mounted archive.

- [`check_bakery_script.py`](../../tools/checks/check_bakery_script.py): a
  two-file module builds in about 0.2 s, its library loads and returns the
  expected value, a warm build runs nothing, a header edit recompiles both
  units, a source edit one unit, errors and warnings carry their codes and
  positions, a failed build keeps the published library, and a bundle ships
  the archive byte-identically.

Unavailable: a Bistro diffuse bake (memory limit of the measurement host),
native Vulkan execution of the catalog path (MoltenVK 1.2), the daemon and a
Vulkan bundle on Windows, Windows script modules, a bundle of a managed
project, a runtime that loads script modules, and an editor frame-time measurement with the daemon running; the
editor-side cost is one non-blocking socket read per frame, but it is not
measured.

## Alternatives considered

- **Keep the Python runner beside a C cache.** Rejected: two implementations of
  the managed document contract and a Python dependency for the editor.
- **In-process cookers.** Deferred: the cookers' globals and fatal paths would
  need an audit first.
- **Per-workspace caches.** Rejected: identical sources in several workspaces
  would encode repeatedly.
- **GPU or system ASTC encoders for final textures.** Rejected for now
  (2026-09-28): Apple's AppleTextureEncoder encoded Bistro colours at 445
  Mpx/s, deterministically, but 2-3 dB below the UASTC `faster` bar;
  astcenc limited to the modes real-time GPU encoders search (one partition,
  no dual plane, 10% of block modes) also fell below it (52.7 against about
  53.5 dB). A GPU encoder that passes would need most single-partition
  modes, which is where the time goes. The system encoder is accepted for the
  editor-only fast speed instead.

## Revisit when

A cooker becomes safe to run in-process with a measured benefit, the renderer
gains pipeline recreation for shader hot reload, a shipped game needs managed
project bundles or streamed archives, or a second consumer needs derived
textures as actions.
