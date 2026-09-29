---
status: proposed
updated: 2026-09-29
authority: proposal
---
# Project build and packaging: remaining scope

Phase 1 shipped: `game.json`, portable lowering, project-mode
`vkr_bakery bundle`, the `vkr_player` template, `bundle.json` version 2 and the
editor Build menu, settings, progress, report and Cmd commands.
[ADR-078](../adr/078-project-build-and-packaging.md) records the contract and
its evidence. This proposal keeps what remains.

## Current baseline

- A package is `<executable>`, `bundle.json`, one backend's shader catalog and
  two version 2 archives, `content/game.vkpak` and `content/engine.vkpak`
  ([package](../../tools/bakery/vkr_bakery_package.c)). Chunks the runtime
  reads into memory are zstd-compressed. An unchanged archive is cloned from
  the previous package; a changed one is rewritten in full.
- The bakery finds a player template beside itself in `templates/player`
  before the build tree's, but no install layout yet assembles a distributed
  editor with one.

## Remaining work

1. **Incremental packages.** Reuse unchanged chunks inside a changed archive,
   and one archive per scene for streaming.
2. **Shipping polish.** Windows icon and version resources; game icons in
   the `.app`; notarization of a Developer ID signed `.app`; linking script
   module archives once the [entity behavior](entity-behavior-system.md)
   runtime loads them.
3. **Editor distribution.** Install rules that lay out the editor,
   `vkr_bakery` and `templates/player` (players, engine resources, shader
   catalog and a relative `template.json`) as one folder.

Cross-compiling a package for another platform stays out of scope: Metal
libraries need the macOS toolchain and the Vulkan catalog is produced per host.

## Evidence needed

- Windows/Vulkan: import `assets/models/bistro.gltf` into a scratch workspace,
  build the Shipping profile, and run the package in normal Release with
  graphics validation unset and `VKR_AUTOCLOSE_SECONDS` set; it exits 0 and
  `$VKR_VFS_RECORD` names only package products.
- A macOS package copied to another volume runs the same way.
- An incremental package needs byte-identical content to a full build and a
  matched Release timing comparison with the event logs retained (ADR-077).

Ask before changing the `.vkpak` format, the ADR-069 workspace layout, or the
set of engine resources a template carries.
