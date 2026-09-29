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
  two version 1 archives, `content/game.vkpak` and `content/engine.vkpak`, each
  rewritten in full by every build
  ([package](../../tools/bakery/vkr_bakery_package.c)).
- The player template is the build tree's `<build>/player`, and its
  `template.json` names that tree's shader catalog by absolute path.

## Remaining work

1. **Incremental packages.** Reuse unchanged chunks and archives, one archive
   per scene for streaming, and per-chunk compression. Compression changes
   `.vkpak` to version 2 and needs approval first.
2. **Shipping polish.** Windows icon and
   version resources; the macOS `.app` layout and signing; linking script module
   archives once the [entity behavior](entity-behavior-system.md) runtime loads
   them.
3. **Distributed templates.** A distributed editor ships `templates/` beside
   itself with the player, engine resources and shader catalog, and the
   bakery looks there before the build tree.

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
