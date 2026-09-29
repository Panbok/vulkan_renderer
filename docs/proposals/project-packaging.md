---
status: proposed
updated: 2026-09-29
authority: proposal
---
# Project build and packaging: remaining scope

Shipped: `game.json`, portable lowering, project-mode `vkr_bakery bundle`, the
`vkr_player` template, `bundle.json` version 2, the editor Build menu,
settings, progress, report and Cmd commands, window modes, signed macOS `.app`
packages, reuse of unchanged archives, zstd-compressed `.vkpak` version 2
chunks, and the relocatable editor distribution.
[ADR-078](../adr/078-project-build-and-packaging.md) records the contract and
its evidence. This proposal keeps what remains.

## Current baseline

- A package is `<executable>`, `bundle.json`, one backend's shader catalog and
  two version 2 archives, `content/game.vkpak` and `content/engine.vkpak`
  ([package](../../tools/bakery/vkr_bakery_package.c)). Chunks the runtime
  reads into memory are zstd-compressed. An unchanged archive is cloned from
  the previous package; a changed one is rewritten in full.
- `build_editor_dist.sh` installs a relocatable editor distribution with its
  bakery, harness, engine content and player template (ADR-078).

## Remaining work

1. **Incremental packages.** Reuse unchanged chunks inside a changed archive,
   and one archive per scene for streaming.
2. **Shipping polish.** Windows icon and version resources; game icons in
   the `.app`; notarization of a Developer ID signed `.app`; linking script
   module archives once the [entity behavior](entity-behavior-system.md)
   runtime loads them; an editor distribution as a signed `.app` or Windows
   installer.

Cross-compiling a package for another platform stays out of scope: Metal
libraries need the macOS toolchain and the Vulkan catalog is produced per host.

## Evidence needed

The Windows code of this work (window modes, package staging, distribution
install) has not been compiled; every gate below except the macOS ones needs a
Windows machine.

- Windows/Vulkan: import `assets/models/bistro.gltf` into a scratch workspace,
  build the Shipping profile, and run the package in normal Release with
  graphics validation unset and `VKR_AUTOCLOSE_SECONDS` set; it exits 0 and
  `$VKR_VFS_RECORD` names only package products.
- A macOS package copied to another volume runs the same way, and a package
  and an editor distribution run on a Mac without the Vulkan SDK or Homebrew.
- A Windows editor distribution from `build_editor_dist.bat` builds a package
  from a moved folder.
- An incremental package needs byte-identical content to a full build and a
  matched Release timing comparison with the event logs retained (ADR-077).

Ask before changing the `.vkpak` format, the ADR-069 workspace layout, or the
set of engine resources a template carries.
