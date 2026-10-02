---
status: proposed
updated: 2026-10-02
authority: proposal
---

# Script modules: deferred edits, packages, threading and packaging

## Baseline

[ADR-079](../adr/079-c-script-modules.md) defines the script SDK (`sdk.h`)
and the runtime host. It covers:

- temp, scoped and persistent lifetimes released through ledgers, with owner
  and timed lifetimes and the transient mark;
- behaviors per entity with destroy hooks, and script instances per attached
  container;
- structural edits in fixed updates, queued until the tick ends;
- library packages with dependencies, built into one project library the
  editor loads, with hot reload that keeps instance data;
- the Script editor, Script objects and the Player Start;
- the FPS sample as a statically linked module.

Bakery builds a project's shared library and static archive
([ADR-077](../adr/077-asset-build-system.md)). This proposal covers what
remains. The SDK still has these gaps:

- **Additive scenes run no scripts.** Their simulation stays paused, so a
  session attaches only the played container and the World.
- **No exports.** Packages share code through library packages; a module
  cannot call into another module.
- **Main-thread work left.** Every hook and a library's first load run on
  the frame thread, as do physics steps that move fewer than 512 active
  bodies. Builds, reloads, model loads, script tasks and larger physics
  steps already run on workers.

## Accepted direction (2026-10-02)

The user accepted these choices; phases 1 to 3 shipped in ADR-079:

- ABI v2 is replaced outright, without a compatibility layer.
- The public header is `sdk.h`.
- Persistent resources live as long as their script instance's container.
- Per-entity behaviors plus systems form the programming model.
- Static library packages come first, and exports later.
- Jolt threading is in scope.
- One library per project is the link unit (accepted 2026-10-02).

### More containers

Additive scenes join a session once their simulation runs, each with its own
instances, and their unload detaches them as the World's does.

### Packages and the project library

Shipped in ADR-079 as described below, except named groups.

- **Kinds.** A `Scripts/<Name>/` folder is a package of kind `module` or
  `library`.
  - Modules declare entry points (behaviors and systems) and are the only
    registered units.
  - Libraries have no entry points; a package lists the libraries it uses in
    `dependencies`, which adds their include roots.
- **One library per project.** Bakery compiles every package's sources to
  objects and links them into one `<project>` library, which exports one
  entry listing its modules. A project may later split into a few named
  groups, like Unity's assemblies, each its own library.
  - One C runtime and one load per project instead of one per package, so
    hundreds of scripts cost one library's startup code, measured at about
    105 KB per Windows DLL.
  - Imports between packages are ordinary linking, and library code and
    state exist once.
  - The host's fixed caps (16 modules, 8 exports per module) become growable
    tables.
- **Builds.** Bakery orders packages by dependency, refuses cycles, compiles
  changed objects only and relinks the project library.
- **Reload.** A project library reload swaps every module in it at once, each
  under the existing rules: unchanged data shapes keep their data, changed
  ones restart the session and changed component layouts are refused.
- **Failures.** A compile error anywhere keeps the previous project library
  running until it is fixed; splitting into groups is the way to isolate
  failing work.
- **Rejected:** one DLL per package with a hybrid C runtime (Windows' shared
  UCRT with static startup code). Failures would stay isolated, but each
  package pays a load and its startup code, and lld-link refused the hybrid
  link with duplicate UCRT symbols in a first attempt.

### Threading

**Stays on the main thread:**

- applying structural commands and hierarchy changes;
- input observation and the window;
- publishing the camera, HUD and UI;
- publishing GPU resources;
- swapping and unloading libraries;
- starting and stopping instances.

**Moves off the main thread:**

- Copying and opening libraries for a reload. Shipped in ADR-079;
  validation stays on the frame thread because it reads the registered
  types.
- `vkr_spawn_model` loads, through the resource system's asynchronous path.
  Shipped in ADR-079 and ADR-080.
- The first build of an unbuilt project. The World request waits for its
  types instead of the frame blocking. Shipped in ADR-079.
- Script tasks, recorded in the ledger. Shipped in ADR-079: a scope's end
  waits for its tasks, and Stop waits for all of them before superseded
  libraries close.
- Jolt's internal work, on `VkrJobSystem` workers. Shipped in ADR-072 for
  steps that move at least 512 active bodies; see
  [Jolt on workers](#jolt-on-workers).

Parallel behaviors and exports come last, when a measured case needs them.

### Jolt on workers

A first `JPH::JobSystemWithBarrier` adapter over `VkrJobSystem` lost to one
thread up to 512 bodies. The user then chose the recommended order:

1. Jolt's large-island splitter: no measurable gain; it stays off.
2. Cheaper work around the jobs: solid contact tracking became linear in the
   contact count (it searched every recorded contact per report and per
   comparison), and `VkrJobSystem` stopped waking every idle worker on each
   completion.
3. Jolt's own pool: not needed; the adapter matches it from 512 bodies.

ADR-072 records the adapter, the 512-body threshold and the measurements.

### Phases

1. Shipped in ADR-079: `sdk.h`, `VkrCtx`, the temp arena, ledgers,
   `vkr_fail`, behaviors and systems, and container and World instances,
   with the FPS module, the template and the tests ported.
2. Shipped in ADR-079: deferred structural edits in fixed updates over ECS
   ID reservation, owner and timed lifetimes, and the transient mark.
3. Shipped in ADR-079: library packages, dependencies and the project
   library in Bakery, the host, the editor and completion.
4. Shipped in ADR-079: asynchronous library reloads, `spawn_model` and first
   builds, and script tasks; Jolt on workers shipped in ADR-072.
5. Exports and parallel behaviors.

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
3. **Headers for projects.** Shipped in ADR-079: the script SDK headers are
   staged as one include root and installed as `sdk/` in the editor
   distribution: `sdk.h`, `defines.h`, `vkr_pch.h` and five math headers.
4. **Shell split.** `vkr_sample_runtime.c` still mixes the game shell with
   editor tooling: gizmo, picking, the edit journal, transport and view
   modes, IBL validation and telemetry. The packaged player links all of it.
   The game shell should keep loading, the World and overlays, the script
   host and the camera, and editor tooling should move to the editor. Only
   overlay loading and applying from the edit journal belong at runtime.
5. **TypeScript.** Describe the SDK once and generate its table, the inline
   calls, the bindings and a `.d.ts` from that description. Implement the language as a
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

- CPU suites show that:
  - a library change rebuilds and reloads its dependents;
  - a reload joins the module's jobs before closing its library.
- Bistro Play and Stop in the headless editor return to the authored entity
  count.
- The Jolt pool's frame-time effect comes from matched Release Bistro
  reports.
- The editor builds, loads and hot reloads a project module on macOS with the
  SDK, as it does on Windows.
- A packaged Bistro game runs its bundled module on both platforms.
- The shell split leaves the player without editor tooling objects.
- A TypeScript behavior matches its C equivalent on Bistro, with measured
  per-call and per-frame cost.
