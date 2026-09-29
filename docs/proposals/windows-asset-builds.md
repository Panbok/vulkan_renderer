---
status: proposed
updated: 2026-09-29
authority: proposal
---
# Windows asset builds

Remaining Windows import work after native BC7/BC5 derived textures, hard-linked
workspace copies, extent-duplication clones and the x86 SHA-256 path shipped
([ADR-012](../adr/012-texture-compression-pipeline.md),
[ADR-077](../adr/077-asset-build-system.md), which records the measurements).
Native renderer evidence stays in the
[Windows/Vulkan checklist](windows-vulkan-verification.md).

## Handoff notes for the implementer

- Load `vkr-task-workflow`, then `vkr-memory` for file lifetime,
  `vkr-harness` for pixel evidence and `vkr-docs` when a part ships.
- Use Bistro for every measurement, import and render comparison.
- One heavy process at a time. Every speed claim needs matched Release runs on
  the same Windows host with a fresh workspace and cache for each cold run, in
  both orders, with the host's CPU, cores, memory, disk and file system.

## Current baseline

On a Ryzen 5 2600 (Windows 10 Pro, NTFS on NVMe) a cold Bistro
`finalize_textures` takes about 48 s at the editor's `bc-fast` speed (460
CPU-seconds) and 122 s at `bc`, a repeat finalization about 6 s, a cold
deferred import about 5.4 s (snapshot 2.2 s, of which the 1.2 GB copy of user
sources is 1.05 s; cook 2.3 s; scene validation and opening 1 s) and a
re-import about 4.7 s. The M1 finishes the same finalization in
about 32 s at `astc-fast` (194 CPU-seconds), a deferred import in 1.6-1.9 s
and a repeat finalization in 5.6 s.

## Proposed change

1. **Remaining cold-finalization CPU.** bc7e is about half (colour mode 6
   and the data-mask rotation modes), then the specular-glossiness
   conversion (about 12%, mostly memo hits), paired-bake moments (5%), PNG
   inflation and the kernel's remaining page faults. The last 9 s of a cold
   finalization run below full occupancy while the last materials encode
   and the cook rehashes every dependency (item 2). Faster colour or mask
   profiles are quality decisions for ADR-012. A GPU BC7 encoder would need
   a port of bc7e's search: DirectXTex's DirectCompute encoder lost to bc7e
   on this host (ADR-077).
2. **The cook's dependency hashes.** The glTF cook still reads and SHA-256
   hashes every bundle dependency each run (0.7 s on eight threads of a warm
   finalization's 4.2 s cook); the parent's index already holds most of those
   digests, and lib's `FileStats` lacks the file identity and nanosecond time
   a hint would need to be trusted.
3. **The deferred cook.** It takes about 2.3 s here against 1.3 s on the M1,
   mostly single-threaded scene emission, tangents, deduplication and vertex
   encoding. The snapshot before it still checks each dependency with about
   ten serial file-metadata calls (0.34 s for Bistro's 339) and writes the
   rewritten glTF (0.23 s).
4. **Scan cost.** Defender's filter adds about 0.4 ms to each rename and its
   service used 54 CPU-seconds over one import and finalization (ADR-077).
   Compare an import with the workspace and cache excluded, and on a Dev
   Drive in performance mode (Windows 11), to decide whether the editor
   should recommend either. Do not disable scanning from code.
5. **Extent-duplication clones.** On a ReFS or Dev Drive volume, measure
   snapshot and revision placement against the NTFS hard links and confirm
   published bytes are identical to a copy.
6. **BC re-render determinism.** Two BC renders of one scene differed in one
   pixel (77 dB) while UASTC renders were identical; find whether texture
   streaming order or the upload path causes it.
7. **Windows check portability.** `check_editor_scene_deletion.py` and
   `check_editor_project_jobs.py` fail on Windows before and after this work
   (line endings and path separators in the checks);
   `check_editor_workspace_cleanup.py` needs the symbolic-link privilege.
8. **Other hosts.** bc7e is built for x86-64 only; an ARM build (ISPC
   `neon-i32x4`) would make `bc` available on Apple silicon, whose Metal GPUs
   sample BC, and needs a Metal load of a BC texture.

## Decision boundaries

- UASTC stays the format of repository `.vkt` files and bundles; `bc` and
  `astc` are workspace formats built for the host.
- Per-file flushes stay (2026-09-28); relaxing them needs an index that
  rehashes after an unclean shutdown.
- User files are always copied into snapshots, never linked.

## Evidence needed

- The scan comparisons above, with the numbers in ADR-077.
- For any profile change, per-class PSNR and Mpx/s on dumped Bistro pre-encode
  data against the current profile and UASTC `faster`, and a Bistro render
  comparison through the harness.
