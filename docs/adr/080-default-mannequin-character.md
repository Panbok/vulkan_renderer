---
status: implemented
updated: 2026-09-30
authority: adr
---

# ADR-080: The default mannequin character

## Status

Accepted. A project's player is VKR's own UE5-style mannequin unless the
project places a model of its own. The FPS module spawns it at a Player Start,
drives it with speed-synchronized locomotion and, in third person, turns it
towards its movement. Every game carries it in its engine content. The limits
under Consequences are quality and evidence gaps, not missing integration.

## Context

Testing movement, jumps, crouching and cameras needs a humanoid of the right
size with walk, jog, run, jump and crouch motion. Before this decision:

- A Player Start spawned a 0.6 × 1.8 × 0.6 m blue box.
- The default templates' empty Player Spawn showed no body at all in third
  person.

Unreal Engine's Manny is the reference. The repository is public, though:

- Epic's content may only be used in Unreal Engine projects.
- Marketplace characters and Mixamo files may not be redistributed as files.

The animation graph ([ADR-071](071-animation-bank-and-reference-pose.md)) runs
each state on a clock with a fixed cycle length. It cannot keep walk, jog and
run loops of different durations on the same footfall.

## Decision

### Content generated from pinned, redistributable sources

[`tools/blender/mannequin`](../../tools/blender/mannequin/build_mannequin.py)
builds the character in background Blender. Its inputs are the 76 files that
[`sources.json`](../../tools/blender/mannequin/sources.json) pins by URL and
SHA-256; `fetch_sources.py` downloads and verifies them.

- **Body.** MakeHuman's CC0 base mesh, macro and region targets (an athletic
  1.80 m recipe), rig and skin weights, at revision `a8bc2d54`. The visor head
  and boots are new ring-swept patches over implicit shapes, grown from the
  base mesh's neck and ankle loops.
- **Skeleton.** 79 bones with UE5 names: `root`, 71 deforming bones (with
  twist bones and metacarpals) and the seven `ik_*` virtual bones. The
  MakeHuman weights are remapped onto it, with four influences per vertex.
- **Surface.** One Catmull-Clark subdivision gives 47,529 vertices and 91,300
  triangles in three PBR materials: the clearcoated grey
  `MI_Mannequin_Shell`, `MI_Mannequin_Visor` and `MI_Mannequin_Boot`.
- **Motion.** 18 clips:
  - `Idle` and `Crouch_Idle`;
  - `Walk_*` and `Jog_*` in four directions (`Fwd`, `Bwd`, `Left`,
    `Right`), plus `Run_Fwd`;
  - `Crouch_Walk_*` in four directions;
  - `Jump_Start`, `Jump_Loop` and `Jump_Land`.

  Idle, backward, sidestep and crouch motion comes from 100STYLE (CC BY 4.0).
  Forward walk, jog, run and the jump come from the CMU database (subjects 9,
  13 and 35).
- **Retargeting**
  ([`mannequin_motion.py`](../../tools/blender/mannequin/mannequin_motion.py)):
  1. Maps world-rotation deltas onto the mannequin.
  2. Aligns the limbs to the source T-pose and calibrates the spine to the
     actor's idle.
  3. Cuts each loop between left heel strikes, so every loop starts on the
     same footfall.
  4. Removes root travel and closes the loop.
  5. Plants the feet: touchdown anchors, heel-to-ball roll and swing
     clearance, with no ground penetration.
- **Check.**
  [`check_mannequin_motion.py`](../../tools/blender/mannequin/check_mannequin_motion.py)
  evaluates Blender's actions, which is what the glTF exporter bakes. A clip
  fails when:
  - its planted feet slip faster than 0.05 m/s at its recorded speed;
  - its loop seam exceeds 0.5°;
  - it goes more than 15 mm below the ground.
- **Output.** A deterministic GLB, plus `mannequin.clips.json` with each
  loop's ground speed, travel direction and duration.

[`publish_mannequin.py`](../../tools/blender/mannequin/publish_mannequin.py) is
the only writer of the content:

- It cooks the GLB with explicit Bakery tool runs: `vkr_bakery tool mesh
  --bundle-root`, which puts the materials in `./materials` beside the `.vkb`,
  and `vkr_bakery tool animation`.
- It replaces [`assets/characters/mannequin`](../../assets/characters/mannequin/NOTICE.md)
  (GLB, `.vkb`, `.vka`, materials, clip table and `NOTICE.md`).
- It writes [`fps_mannequin.h`](../../scripts/fps/src/fps_mannequin.h), the
  paths and clip table the FPS module compiles in.

Build wrappers never run Blender. Changing the mannequin means republishing
and committing the content with its header.

`NOTICE.md` credits every source. The 100STYLE licence requires attribution,
so a game that ships the clips must carry "The 100STYLE Dataset - Ian Mason"
in its credits or bundled notices. Every package's `engine.vkpak` carries
`NOTICE.md`.

### Spawned models

[`vkr_scene_spawn_model`](../../runtime/src/renderer/systems/vkr_scene_model.h)
loads a cooked mesh and an optional animation bank synchronously. It
instantiates the mesh's source nodes under a live wrapper entity, as a scene
document would, and creates:

- the node entities;
- one mesh instance per mesh node, casting dynamic shadows;
- the animation binding (ADR-071).

Constraints:

- The wrapper needs a transform and may carry no mesh or animation of its own.
- One model per wrapper.
- Spawning happens only between simulation ticks.

The scene owns what the spawn created. `vkr_scene_despawn_model`, destroying
the wrapper or scene shutdown releases it, and a failed spawn leaves nothing.

`vkr_scene_set_local_matrix` on an entity without a transform now validates it
as a root while physics bodies exist, as `vkr_scene_set_transform` already
did. Before, it refused every new entity, so no model could spawn during Play.

[`VkrScriptApi`](../../runtime/src/script/vkr_script.h) appends four members;
the ABI version stays 2:

- `spawn_model` and `despawn_model`;
- `animation_sample_blend`;
- `renders_mesh`: whether the entity or a descendant carries a mesh or a
  shape.

### The player's body

At start the [FPS module](../../scripts/fps/src/fps_module.c) chooses the body:

- **No `fps_player` entity.** A Player Start spawns `Player`, whose child
  `PlayerBody` holds the mannequin.
- **An `fps_player` entity with an animation, mesh or shape** on it or below
  it is its own body. Placing a model and attaching the script replaces the
  mannequin.
- **An empty `fps_player` entity**, such as the templates' Player Spawn, gets
  a `PlayerBody` mannequin.
- **A mannequin that fails to load** logs a warning, and the body falls back
  to the box.

Stop destroys what the module spawned.

### Locomotion

[`FpsLocomotion`](../../scripts/fps/src/fps_locomotion.h) drives a bank with
the mannequin's clip names (at least `Idle`, `Walk_Fwd` and `Jump_Loop`)
through `animation_sample_blend`, on its own clocks. The player stays paused,
so the scene clock never replaces the pose. Other banks keep the named action
clips of `FpsPlayerAnimation` ([ADR-073](073-native-gameplay-foundation.md)).

- **One phase.** Every loop starts on the same footfall, so one normalized
  phase drives them all. The phase advances by distance covered over the
  blended stride (each loop's speed × duration), so planted feet stay still at
  any speed. Above the fastest loop, the run plays faster.
- **Directions and gaits.**
  - Velocity in the body's frame, smoothed over 0.08 s, splits into
    forward, backward, left and right shares.
  - Each direction blends idle into walk, jog and run by speed. A missing
    loop falls back to the forward walk.
  - Crouching blends in over 0.2 s, with its own idle and walks.
- **Air.**
  - Leaving the ground while rising plays `Jump_Start` into `Jump_Loop`;
    falling plays `Jump_Loop`.
  - Touching down after at least 0.25 s in the air plays `Jump_Land`, which
    moving cuts short.
  - The air, landing and ground shares sum to one.
- **Presentation rate.** Ticks record the solved velocity and support.
  [Presentation](../../scripts/fps/src/fps_player.c) advances the pose to the
  render time of the interpolated root, (ticks − 1 + α) / 60 s, so planted
  feet stay under the drawn body at any frame rate. Posing on the tick would
  put the pose up to one tick away from the drawn root: 8 cm at 5 m/s.

### Third-person movement

`fps_player` gains four properties:

- **Walk speed:** 1.5 m/s while Shift is held.
- **Third person group:**
  - Orient to movement: on.
  - Acceleration: 14 m/s²; braking is a third faster.
  - Turn rate: 540°/s.

In third person with Orient to movement, the body turns towards its movement
and speed changes ease. Other views strafe with the camera and change speed
at once, as before.

### Engine content

[`vkr_engine_content.cmake`](../../cmake/vkr_engine_content.cmake) adds the
`.vkb`, the `.vka` and `NOTICE.md` as engine closure roots, and the three
materials as engine files. The player template, every package's
`engine.vkpak` and the editor distribution's `content/` carry them: 3.4 MiB of
inputs.

## Consequences

- Every project shows a character in third person without authoring one. An
  animated, static or shape model with `fps_player` replaces it.
- Every package grows by the mannequin whether it uses it or not, because the
  FPS module is linked into every player.
- The clip table in `fps_mannequin.h` must match the published bank.
  Republishing updates both. A bank without the table's names falls back to
  the action clips.
- A game that ships the clips must credit 100STYLE.
- The 100STYLE backward and sidestep loops travel at 0.47 to 1.49 m/s. With
  Orient to movement off, strafing at 5 m/s plays them up to 4.5 times faster:
  the feet stay planted, but the cadence is high.
- The pose is evaluated on the CPU every presented frame, and 91,300
  triangles are skinned in compute every frame. No timing has been measured.
- Not implemented:
  - foot IK on slopes and steps;
  - turn-in-place;
  - aim offsets and upper-body layers;
  - LODs;
  - surface detail textures (the materials are flat colours).

## Alternatives considered

- **Epic's Manny and Quinn.** Their licence limits them to Unreal Engine
  projects, so this public repository cannot carry them.
- **Mixamo characters and animations.** Their terms forbid redistributing the
  files themselves.
- **One animation graph state per gait.** A state's fixed cycle length cannot
  phase-sync loops of different durations, and blending unsynchronized loops
  slides the feet.
- **A World Settings default-character field.** Deferred: placing a model with
  `fps_player` already covers replacement.
- **CC0-only motion libraries.** They offer stylized proportions, or no clean
  locomotion cycles at the needed speeds.

## Revisit when

- Projects need a character asset setting, or prefabs spawn models (the
  [behavior proposal](../proposals/entity-behavior-system.md)).
- Faster backward and strafe motion capture becomes available, or foot IK,
  turn-in-place or aim layers are added.
- A timing gate puts the mannequin's skinning cost against the frame budget.

## Evidence

macOS 26.6.2, Apple M1 Pro, Release, Metal, Blender 5.2.2, 2026-09-30:

- **Sources.** `fetch_sources.py` downloaded and verified all 76 inputs; a
  second run downloaded nothing.
- **Motion check.** `check_mannequin_motion.py` on the published build passed
  all 18 clips:
  - worst planted-foot slip: `Walk_Fwd`, p95 0.038 m/s (max 0.041);
  - `Jog_Fwd`: p95 0.017 m/s;
  - every other clip: p95 at most 0.005 m/s;
  - loop seams at most 0.051°; lowest point −5.7e-7 m.
- **Builds.** `./build_editor.sh Release` and
  `VKR_BUILD_TARGET=vulkan_renderer_tester ./build.sh Release` build with no
  compiler warnings, and `build_release/tests/vulkan_renderer_tester` passes:
  - `scene_animation_tests.c`: spawning refuses a wrapper without a transform
    and a second model on one wrapper. Despawning and destroying the wrapper
    both release the nodes and pool blocks. A bank cooked from another source
    fails and leaves nothing behind.
  - `player_animation_test.c` runs locomotion through a real player:
    - A loop at its authored speed plays at 1×.
    - Midway between walk and jog, the weights are 0.5 and 0.5 on one phase,
      which advances by the blended stride.
    - Past its loop speed, the run plays faster.
    - Sidestepping works, including the fallback for a missing loop.
    - Jump start, loop and land take the right shares, and no ground loop
      leaks into the landing.
    - A short hop lands without `Jump_Land`; crouching blends in.
    - The pose follows render time.

    Restoring the landing's double-counted air share fails the suite, and so
    does posing one tick ahead.
  - `script_host_test.c`: `renders_mesh` finds a descendant mesh and a shape,
    and returns false for an empty entity.
- **ASan.** The Debug (AddressSanitizer) tester passes every suite under
  `ASAN_OPTIONS=report_globals=0`. Without that option, the reload suite hits
  its known ASan global fault ([ADR-079](079-c-script-modules.md)).
- **Template project.** RPG Grounds from `check_default_templates.py`, in the
  headless editor:
  - Play went from 472 to 554 entities: `PlayerBody` and 81 source nodes.
  - Stop restored 472, with no warnings.
- **Bistro, headless.**
  - A created Player Start spawned `Player` with the mannequin (+83
    entities), and Stop restored the scene.
  - A cube with `fps_player` stayed its own body.
  - An empty entity with `fps_player` got a `PlayerBody` (+82 entities).
- **Package.** `vkr_bakery bundle` of the RPG Grounds project wrote an
  `engine.vkpak` of 15 entries (17.0 MiB), including the six mannequin files.
  `check_bakery_package.py` passes.
  - The packaged game showed the mannequin idling at the Player Start in third
    person.
  - Two frames 0.7 s apart differed in 2,330 pixels of the character's region,
    against 0.07 % of the sky band.

Not run:

- Windows and Vulkan;
- movement driven from the keyboard in a window (no synthetic input was
  sent);
- timing.
