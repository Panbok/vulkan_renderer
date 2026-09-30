---
status: proposed
updated: 2026-09-30
authority: proposal
---

# Script modules: packaging, SDK headers, TypeScript and the shell split

## Baseline

[ADR-079](../adr/079-c-script-modules.md) defines the C script ABI and the
runtime host. It also covers:

- loading project libraries in the editor, with hot reload that keeps state;
- the Script editor;
- Script objects and the Player Start;
- the FPS sample as a statically linked module.

Bakery builds each module's shared library and static archive
([ADR-077](../adr/077-asset-build-system.md)). This proposal covers what
remains.

## Remaining work

1. **Component migration.** Hot reload refuses a component whose fields
   changed; the project must be reopened, and nothing migrates the old bytes.
   A migration needs versioned field serialization and atomic replacement of
   the live values.
2. **Packaged games.** The recommended first step ships the shared library
   beside the prebuilt `vkr_player`, signed inside the `.app`. The player
   then loads the modules `bundle.json` names instead of linking the FPS
   module. Consoles and iOS need static linking later. That requires a linker
   at package time and runtime archives and headers in the editor
   distribution.
3. **Headers for projects.** Bakery and the Script editor's completion read
   the engine headers from this source tree (`VKR_BAKERY_SCRIPT_SDK_DIRS`,
   `VKR_EDITOR_SCRIPT_SDK_ROOT`). The editor distribution needs the runtime
   and foundation headers, or a trimmed script SDK header, so project scripts
   compile outside the repository.
4. **Shell split.** `vkr_sample_runtime.c` still mixes the game shell with
   editor tooling: gizmo, picking, the edit journal, transport and view
   modes, IBL validation and telemetry. The packaged player links all of it.
   The game shell should keep loading, the World and overlays, the script
   host and the camera, and editor tooling should move to the editor. Only
   overlay loading and applying from the edit journal belong at runtime.
5. **TypeScript.** Describe the API once and generate the C table, the
   bindings and a `.d.ts` from that description. Implement the language as a
   C script module hosting QuickJS-ng. QuickJS-ng is an interpreter with no
   JIT, so it runs on consoles and iOS. Keep per-entity loops in C systems and
   give TypeScript batch calls for event and sparse logic. Measure crossing
   cost before any timing claim.

## How other engines split native and script code

- **Unreal Engine 5.** Uses C++ modules with generated reflection and
  Blueprint bytecode. Verse runs only in UEFN until UE6's Scene Graph.
  ([Verse and UE6 timeline](https://www.strayspark.studio/blog/will-verse-replace-blueprints-unreal-engine-6))
- **Unity.** Moves from Mono to CoreCLR (JIT) and keeps IL2CPP (AOT) for
  restricted platforms. Reloading only changed assemblies keeps static state.
  ([Path to CoreCLR](https://discussions.unity.com/t/path-to-coreclr-2026-upgrade-guide/1714279))
- **Godot.** GDExtension hands a C function table to the extension. Hot
  reload came later and still crashes on stale function pointers.
  ([reload PR](https://github.com/godotengine/godot/pull/80284))
- **Frostbite.** Gameplay runs in C++, with Schematics as data-driven visual
  scripting over native actions and C# tools.
  ([Schematics role](https://gamejobs.co/Software-Engineer-Frostbite-Schematics-C-C-at-Electronic-Arts-795))
- **TypeScript precedents.** Minecraft Bedrock runs QuickJS without a JIT
  ([engine environment](https://wiki.bedrock.dev/scripting/api-environment)).
  PuerTS generates static wrappers and `.d.ts` files over V8 or QuickJS
  ([static wrappers](https://puerts.github.io/en/docs/puerts/unity/wrapper/)).

## Evidence to accept

- The editor builds, loads and hot reloads a project module on Windows as it
  does on macOS.
- A packaged Bistro game runs its bundled module on both platforms.
- The shell split leaves the player without editor tooling objects.
- A TypeScript behavior matches its C equivalent on Bistro, with measured
  per-call and per-frame cost.
