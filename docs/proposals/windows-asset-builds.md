---
status: proposed
updated: 2026-09-28
authority: proposal
---
# Windows asset builds

Bring managed imports on Windows (and other desktop hosts without ASTC) to the
speed macOS reached in [ADR-077](../adr/077-asset-build-system.md): native
BC7/BC5 derived textures in place of UASTC, and the Windows-specific file,
hashing and editor work that the macOS measurements never exercised. The
remaining daemon, bundle and script scope on Windows stays in the
[asset build system proposal](asset-build-system.md); native renderer evidence
stays in the [Windows/Vulkan checklist](windows-vulkan-verification.md).

## Handoff notes for the implementer

- Load `vkr-task-workflow`, then `vkr-memory` for arenas and file lifetime,
  `vkr-renderer-design` for the texture upload path, `vkr-harness` for pixel
  evidence and `vkr-docs` when a part ships.
- Use Bistro for every measurement, import and render comparison.
- One heavy process at a time: never loop cookers or run two imports at once.
  Delete run workspaces after recording their numbers.
- Every speed claim needs matched Release runs on the same Windows host with a
  fresh workspace and cache for each cold run, in both orders. Record the
  host's CPU, core count, memory, disk and file system with the numbers.
- Decisions marked **ask** go to the user with the recommendation and its
  cost before dependent code is written.

## Current baseline

Code and ADRs as of 2026-09-28; nothing below has been measured on Windows.

- **Texture encoding.** Workspace-derived textures use native ASTC 4x4 only on
  Apple silicon (`VKR_PROJECT_NATIVE_ASTC` in
  [project internals](../../tools/bakery/project/vkr_project_internal.h));
  every other host encodes UASTC at the `faster` level
  ([ADR-012](../adr/012-texture-compression-pipeline.md)). On the M1 a
  final-tier Bistro import took 408.4 s with UASTC against 125.6 s with ASTC
  before later work; UASTC `faster` encoded 7.9 Mpx/s on eight cores against
  88 Mpx/s for ASTC `fast`. The runtime transcodes UASTC to BC7 or BC5 at
  load and keeps a transcode cache
  ([texture system](../../runtime/src/renderer/systems/vkr_texture_system.c));
  it uploads only ASTC payloads directly (`vkr_texture_ktx2_decode_astc`).
- **Editor-only fast speed.** `texture_encode_speed: fast` selects Apple's
  system encoder (`astc-fast`, macOS only); UASTC ignores it, so Windows has
  no fast editor encode.
- **Copies.** `file_clone` returns `FILE_ERROR_UNSUPPORTED` on Windows
  ([filesystem](../../lib/src/platform/vkr_filesystem_windows.c)), so every
  source snapshot and bundle copy writes the bytes; APFS clones them on
  macOS (Bistro's sampled sources are about 3.1 GiB).
- **Durability.** Published files are flushed (`FlushFileBuffers`) and renamed
  with `MOVEFILE_WRITE_THROUGH`; one Bistro finalize publishes several hundred
  textures and 254 materials.
- **Hashing.** SHA-256 uses the ARMv8 instructions on Apple silicon and the
  portable C loop elsewhere ([hash](../../lib/src/core/vkr_hash.c)); x86-64
  SHA extensions are unused.
- **Fresh-file cost.** On the macOS host, opening a just-written file cost
  15-30 ms, which shaped the design to avoid rereading outputs. Windows
  Defender's real-time scan of new files is the analogous risk and is
  unmeasured.
- **Editor.** Deferred imports, the background `finalize_textures` and
  `finalize_project_assets` jobs, inventory adoption and the scene reopen are
  verified only on macOS.

## Proposed change

### 1. Windows baseline

Before changing code, measure on one Windows host with the same scripts the
macOS work used (a port of `.scratch/import-perf/run.py`: fresh workspace and
cache, `create_scene` at `texture_tier` `deferred`, then
`finalize_textures`, then a repeat import and finalization): wall, child CPU,
peak memory, the runner's per-stage timings and the model rebuild's share. Add
a CPU sample (Windows Performance Recorder or equivalent) of the cold finalize
and a file-activity trace of one import, so encode, decode, copy, flush and
scan costs are separated before any of them is optimized.

### 2. Native BC7/BC5 derived textures

A new encoding `bc`, parallel to `astc` in
[ADR-012](../adr/012-texture-compression-pipeline.md):

- **Formats.** `color-srgb`, `color-linear`, cutout variants and `data-mask`
  encode BC7 (vkFormat 145 unorm, 146 sRGB); `normal-rg`, including paired
  normals, encodes BC5 unorm (141) from R and G. BC5 keeps two independent
  channels, which ASTC and UASTC approximate through a shared endpoint
  model, so paired normals should gain the most.
- **Selection.** Hosts whose GPUs sample BC, which every desktop Vulkan
  driver does, default to `bc`: Windows and Linux on x86-64. Apple silicon
  keeps `astc` by default; `texture_encoding: bc` stays available there,
  since its Metal GPUs sample BC too.
- **Identity and names.** The settings identity records the encoder, its
  version and profile per class (for example `encoding=bc7-<encoder>-<profile>`
  and `bc5-<encoder>`); file names carry `.bc` and `-bc`, with preview and
  fast variants, so no BC output ever satisfies an ASTC or UASTC recipe.
- **Speeds.** `texture_encode_speed: fast` selects a faster BC7 profile for
  editor-only textures, as `astc-fast` does on macOS; explicit and
  command-line requests use the final profile. The editor already sends
  `fast`.
- **Packer.** `VkrVktEncoding` gains `VKR_VKT_ENCODING_BC` (and a fast value
  if the chosen encoder has one); `write_packed_sources` dispatches to the
  encoder beside `compress_astc` and `compress_astc_system`, reusing
  `create_astc_texture`'s block-texture creation generalized to the target
  format, and `encode_astc_images`' per-image loop. The runner's encoding
  names, suffixes and texture recipes extend the same tables.
- **Runtime.** A direct-upload path for BC7 and BC5 payloads beside
  `vkr_texture_ktx2_decode_astc`: no transcode or transcode-cache entry, and
  a device without BC refuses the texture with the same rebuild instruction.
  Format enums, channel counts and upload sizes already exist for the
  transcode targets.
- **Quality bar.** Per class, at least UASTC `faster`, as ADR-012 required of
  ASTC: 52.2 dB colour, 38.5 dB paired normals (RG) and 47.4 dB
  metallic-roughness on Bistro's pre-encode data, measured by dumping level-0
  texels before encoding (as the ASTC work did) and comparing both encoders
  on the same images. The fast profile has no bar but reports its numbers.

**Ask: the BC7 encoder.** Candidates, all with permissive licences:

| Encoder | BC7 | BC5 | Build dependency | Notes |
| --- | --- | --- | --- | --- |
| bc7enc_rdo (`bc7e.ispc`, `rgbcx`) | yes, ISPC | yes (`rgbcx`) | ISPC compiler | The Basis Universal author's encoder; ISPC also targets ARM, so one encoder serves macOS |
| Intel ISPC Texture Compressor | yes, ISPC | yes | ISPC compiler | Older; profiles from ultrafast to slow |
| AMD Compressonator (`CMP_Core`) | yes, C++ SIMD | yes | none | No extra compiler |
| DirectXTex | yes (CPU, or D3D11 compute) | yes | Windows SDK, D3D11 | Windows only |

Recommendation: bc7enc_rdo (BC7 from `bc7e.ispc`, BC5 from `rgbcx`), built
through CMake's ISPC language support with a prebuilt-object fallback if ISPC
is absent. Aras Pranckevičius's 2020 comparison placed `bc7e` among the best
CPU BC7 encoders for speed at a given quality, and one encoder then serves
every host. Cost: ISPC becomes a
build dependency for the tools. Compressonator is the choice if that
dependency is unacceptable. Select only after measuring speed and per-class
PSNR on Bistro's dumped pre-encode data on the Windows host; keep the
measurement tool in `.scratch/`.

GPU encoders (Betsy's Vulkan compute shaders cover BC1-BC6H but not BC7)
are out of scope; ADR-077 records why GPU ASTC encoders failed the quality
bar, and the same bound applies until an encoder is measured.

### 3. File system

- **Block cloning.** Implement `file_clone` with
  `FSCTL_DUPLICATE_EXTENTS_TO_FILE` on ReFS volumes (Dev Drive), falling back
  to the existing copy on NTFS; the callers already fall back. Evidence:
  snapshot and bundle copy time and bytes written on each file system, and
  identical published bytes.
- **Scan cost.** Measure an import on an NTFS volume with Defender real-time
  protection, on a Dev Drive in performance mode, and with the workspace and
  cache excluded, to decide whether the editor should recommend a Dev Drive
  for workspaces. Do not disable scanning from code.
- **Flush cost.** Measure the time spent in `FlushFileBuffers` and
  write-through renames during a Bistro finalize. **Ask** before relaxing
  per-file durability (for example, one flush per publication): it changes
  what a power loss can leave behind.

### 4. Hashing

Add an x86-64 SHA-256 path using the SHA extensions (available on AMD Zen and
Intel Ice Lake and later), selected by CPUID at startup, beside the ARMv8
path. Evidence: identical digests on the SHA-256 test vectors and on a Bistro
closure, and MB/s before and after.

### 5. Editor flow on Windows

Run the headless editor sequence the macOS work used (`--headless --exec`
with a deferred Bistro scene: open, background finalization, reopen) and a
deferred Content import with `finalize_project_assets`. Confirm the job
cancellation, retry-when-idle and inventory digest checks behave the same.

### 6. Checks

Run `build_test.bat` and the project checks under `tools/checks/`, including
`check_editor_texture_tiers.py` (extend it with the `bc` encoding: names,
identity, vkFormats 141/145/146, and UASTC unchanged by `fast`),
`check_spec_gloss_memo.py`, `check_editor_project_jobs.py`,
`check_editor_workspace_cleanup.py` and the path checks.

## Order of work

1. Windows baseline (section 1).
2. Encoder measurement on dumped Bistro data and the encoder decision.
3. BC encoding in the packer, cooker and runner, then the runtime direct
   upload, then the editor default; measured against the baseline, with the
   render comparison.
4. File-system items in the order the baseline profile ranks them.
5. SHA extensions.
6. Editor flow and checks.

## Decision boundaries

- UASTC stays the format of repository `.vkt` files and bundles; `bc` and
  `astc` are workspace formats built for the host.
- Encoding is part of every texture key and name; outputs of one encoding
  never substitute for another's.
- Portable rendering semantics do not change: shaders sample the same RG
  normal and RGBA channels whatever the block format.
- Apple silicon keeps ASTC as its default unless measurements show BC
  (encode speed and quality) worth a switch; that would be a separate
  decision.

## Evidence needed

- The Windows baseline and every later step measured as described in the
  handoff notes, with the numbers in ADR-077 when shipped.
- Per-class PSNR and Mpx/s for the chosen BC7/BC5 settings and for UASTC
  `faster` on the same dumped Bistro images.
- A Bistro render comparison through the harness (`managed_workspace` case,
  manual exposure 16): BC against UASTC on the same host, with PSNR, mean and
  pixel counts above 10 and 30/255, and a bit-exact BC re-render.
- Native Vulkan validation on the Windows host for the direct BC upload path
  (one focused diagnostic run, per the validation skill).
- Materials and textures of a finalized scene byte-identical to a direct
  final import at the same encoding.

## Code baseline

- [Packer](../../tools/vkr_vkt_packer.cpp) and [its API](../../tools/vkr_vkt_packer.h)
- [Mesh cooker glTF materials](../../tools/assets/mesh_loader_gltf.c)
- [Project runner options](../../tools/bakery/project/vkr_project_main.c),
  [tier and encoding tables](../../tools/bakery/project/vkr_project_util.c),
  [texture producer](../../tools/bakery/vkr_bakery_assets.c)
- [Texture system upload and transcode](../../runtime/src/renderer/systems/vkr_texture_system.c)
- [Windows file system](../../lib/src/platform/vkr_filesystem_windows.c),
  [SHA-256](../../lib/src/core/vkr_hash.c)
- [Editor project jobs](../../editor/src/editor_projects.c)
