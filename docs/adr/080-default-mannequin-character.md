---
status: implemented
updated: 2026-10-01
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
builds the character in background Blender. Its inputs are the 19 files that
[`sources.json`](../../tools/blender/mannequin/sources.json) pins by URL and
SHA-256; `fetch_sources.py` downloads and verifies them and unpacks the
Blender Studio archive.

- **Body** ([`mannequin_base.py`](../../tools/blender/mannequin/mannequin_base.py)).
  The realistic male of Blender Studio's Human Base Meshes v1.4.1 (CC0), at
  its first multiresolution level, scaled to 1.80 m. Polynomial fits and
  masked smoothing turn the anatomy into armour forms:
  - one chest shield and one back shield, a smooth abdomen and glutes;
  - no nipples or navel, and a softened groin;
  - a deeper chest over a narrower waist, and the neck set back over the
    spine.

  The helmet head and the boots are new radial patches over implicit shapes,
  grown from the body's neck and ankle loops. The helmet is 25 cm tall, 18 cm
  wide and 23 cm deep, with an upright faceplate that narrows to a pointed
  chin.
- **Skeleton** ([`mannequin_rig.py`](../../tools/blender/mannequin/mannequin_rig.py)).
  79 bones with UE5 names: `root`, 71 deforming bones (with twist bones and
  metacarpals) and the seven `ik_*` virtual bones.
  - Limb and finger joints are the centres of the rings where the base's
    sculpt face sets meet; the spine follows sections of the torso.
  - Skin weights are Blender's bone heat on the 30,219-vertex base.
  - Rules then make the helmet rigid on `head` down to the jaw line, split
    each boot between `foot` and `ball`, and keep four influences per vertex.
- **Shell** ([`mannequin_shell.py`](../../tools/blender/mannequin/mannequin_shell.py),
  [`mannequin_design.py`](../../tools/blender/mannequin/mannequin_design.py)).
  The design is a table of features in the body's frame. Outlines are drawn
  in orthographic views or in views unrolled around a limb; tubes, bands,
  planes and ellipsoids complete them, each masked by a body region. Every
  feature is one of three things:
  - a plate, raised up to 5.5 mm (8 mm where two layers stack);
  - a groove;
  - a recessed panel of the suit.

  Off-white plates cover a dark carbon suit, which shows in the channels
  between them and at the joints, with steel-blue accents. The layout is
  VKR's own:
  - a faceplate helmet with temple slots, fins, ear discs and vents;
  - a ribbed neck in a collar ring;
  - a chest plate carrying a chevron, above two chevron bands;
  - abdomen, oblique and brief plates;
  - back blades on either side of an accent spine;
  - two-layer shoulder and knee caps;
  - sleeves, forearm guards, cuffs and greaves on the limbs;
  - segmented fingers;
  - screws, hatches, vents and panel lines.
- **Surface** ([`mannequin_bake.py`](../../tools/blender/mannequin/mannequin_bake.py)).
  - The game mesh is the skinned base after one Catmull-Clark subdivision:
    120,950 vertices and 241,896 triangles, displaced by the design's coarse
    steps.
  - A detail mesh of 3.2 M vertices, with the body two levels finer and the
    helmet three, carries every feature.
  - Cycles bakes the detail mesh onto the game mesh through a smooth cage:
    a tangent-space normal map, ambient occlusion and masks, at 2048² with
    the helmet at twice the body's texel density.
  - The composed textures add plate-edge wear and chips, scratches, grime
    in occluded corners, dust on the lower legs, a carbon weave on the suit
    and roughness variation.
  - One PBR material, `MI_Mannequin`, takes the base colour,
    occlusion-roughness-metallic and normal maps.
- **Motion.** 18 clips:
  - `Idle` and `Crouch_Idle`;
  - `Walk_*` and `Jog_*` in four directions (`Fwd`, `Bwd`, `Left`,
    `Right`), plus `Run_Fwd`;
  - `Crouch_Walk_*` in four directions;
  - `Jump_Start`, `Jump_Loop` and `Jump_Land`.

  Idle, backward, sidestep and crouch motion comes from 100STYLE (CC BY 4.0).
  The rest comes from the CMU database:
  - the forward walk at 1.90 m/s, subject 16;
  - the jog at 3.38 m/s, subject 35;
  - the run at 3.81 m/s, subject 9;
  - the jump, subject 13.
- **Retargeting**
  ([`mannequin_motion.py`](../../tools/blender/mannequin/mannequin_motion.py)):
  1. Maps world-rotation deltas onto the mannequin.
  2. Aligns the limbs to the source T-pose and calibrates the spine to the
     actor's idle.
  3. Cuts each loop between left heel strikes, so every loop starts on the
     same footfall.
  4. Removes root travel and closes the loop.
  5. Corrects each clip's mean posture for its gait (`POSTURES`):
     - torso lean: idle 1°, walk 3°, jog 7°, run 10°;
     - the head within 6° of level, facing forward;
     - shoulders at most 5° above rest;
     - elbow flexion: 20° idle, 22° to 25° walking, 45° jogging sideways or
       backwards, 90° jogging and running forwards;
     - hands clear of the hips and, in the forward jog and run, apart in
       front of the chest;
     - in the forward gaits and the jump, both arms moving alike; in the
       walk, a 35 % wider arm swing.

     Every correction except the swing is constant over the clip, so the
     capture's own motion stays.
  6. Plants the feet: touchdown anchors, heel-to-ball roll and swing
     clearance, with no ground penetration.
- **Check.**
  [`check_mannequin_motion.py`](../../tools/blender/mannequin/check_mannequin_motion.py)
  evaluates Blender's actions, which is what the glTF exporter bakes. A clip
  fails when:
  - its planted feet slip faster than 0.05 m/s at its recorded speed;
  - its loop seam exceeds 0.5°;
  - it goes more than 15 mm below the ground;
  - its mean head pitch is more than 8° from level;
  - a hand enters the hips, an elliptic cylinder about the pelvis
    (`hip_intrusion`).
- **Output.** `mannequin.gltf` with its buffer and PNG textures as separate
  files, because the mesh cooker reads image files and not images embedded
  in a GLB. `mannequin.clips.json` records each loop's ground speed, travel
  direction and duration.

[`publish_mannequin.py`](../../tools/blender/mannequin/publish_mannequin.py) is
the only writer of the content:

- It cooks the glTF with explicit Bakery tool runs:
  - `vkr_bakery tool mesh --bundle-root`, which puts the material in
    `./materials` and its textures in `./textures` beside the `.vkb`;
  - `vkr_bakery tool texture` for each source image the material names,
    into its `.vkt` sibling;
  - `vkr_bakery tool animation`.
- It replaces [`assets/characters/mannequin`](../../assets/characters/mannequin/NOTICE.md):
  the source glTF, buffer and images, the `.vkb`, the `.vka`, the material,
  the textures, the clip table and `NOTICE.md`.
- It writes [`fps_mannequin.h`](../../scripts/fps/src/fps_mannequin.h), the
  paths and clip table the FPS module compiles in.
- It writes [`vkr_mannequin_content.cmake`](../../cmake/vkr_mannequin_content.cmake),
  the content's closure roots and the files they reach.

Build wrappers never run Blender. Changing the mannequin means republishing
and committing the content with its header and content list.

`NOTICE.md` credits every source. The 100STYLE licence requires attribution,
so a game that ships the clips must carry "The 100STYLE Dataset - Ian Mason"
in its credits or bundled notices. Every package's `engine.vkpak` carries
`NOTICE.md`.

### Spawned models

[`vkr_scene_spawn_model`](../../runtime/src/renderer/systems/vkr_scene_model.h)
loads a cooked mesh and an optional animation bank synchronously.
`vkr_scene_request_model` starts the same loads on the resource system's
workers instead and reserves the wrapper; the scene's update instantiates
the model at the first frame start where both loads are ready
(`vkr_scene_models_update`), or records the failure, which
`vkr_scene_model_status` reports. Either way the model's source nodes are
instantiated under a live wrapper entity, as a scene document would, with:

- the node entities;
- one mesh instance per mesh node, casting dynamic shadows;
- the animation binding (ADR-071).

Constraints:

- The wrapper needs a transform and may carry no mesh or animation of its own.
- One model per wrapper.
- Spawning happens only between simulation ticks.

The scene owns what the spawn created. `vkr_scene_despawn_model`, destroying
the wrapper or scene shutdown releases it, cancelling a request still
loading, and a failed spawn leaves nothing.

`vkr_scene_set_local_matrix` on an entity without a transform now validates it
as a root while physics bodies exist, as `vkr_scene_set_transform` already
did. Before, it refused every new entity, so no model could spawn during Play.

The script SDK ([`sdk.h`](../../sdk/sdk.h), [ADR-079](079-c-script-modules.md))
serves the mannequin with:

- `vkr_spawn_model`, which requests the model, `vkr_model_state`, which
  reports it loading, ready or failed, and `vkr_despawn_model`; the spawn is
  released with the calling scope;
- `vkr_anim_blend`;
- `vkr_has_visual`: whether the entity or a descendant carries a mesh or a
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

The mannequin loads in the background: the player moves at once without a
visible body, and its locomotion binds on the first tick after the model
arrives. An `fps_weapon` bone is checked against the skeleton then.

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

[`vkr_engine_content.cmake`](../../cmake/vkr_engine_content.cmake) includes
the generated list:

- the `.vkb`, the `.vka` and `NOTICE.md` are engine closure roots;
- the material, its four cooked textures and the two source images it names
  are engine files.

The player template and the editor distribution's `content/` carry the
engine files. A package's `engine.vkpak` carries the closure, each named
image as its `.vkt`: eight mannequin entries, 28.4 MiB.

## Consequences

- Every project shows a character in third person without authoring one. An
  animated, static or shape model with `fps_player` replaces it.
- Every package grows by the mannequin whether it uses it or not, because the
  FPS module is linked into every player: the textured mannequin takes the
  `engine.vkpak` from 17.0 MiB to 42.0 MiB.
- The occlusion-roughness-metallic image is cooked twice. The mesh cooker
  converts it into the metallic-roughness texture and leaves the occlusion
  slot on the source image, which publishing cooks separately, so 5.3 MiB of
  every package repeats.
- The clip table in `fps_mannequin.h` must match the published bank.
  Republishing updates both. A bank without the table's names falls back to
  the action clips.
- A game that ships the clips must credit 100STYLE.
- The 100STYLE backward and sidestep loops travel at 0.44 to 1.39 m/s. With
  Orient to movement off, strafing at 5 m/s plays them up to 4.9 times faster:
  the feet stay planted, but the cadence is high. The forward run plays 1.3
  times faster at 5 m/s.
- The pose is evaluated on the CPU every presented frame, and 241,896
  triangles are skinned in compute every frame. No timing has been measured.
- A build takes about three minutes and 5.3 GB of memory, most of both in the
  detail mesh and its bake.
- Not implemented:
  - foot IK on slopes and steps;
  - turn-in-place;
  - aim offsets and upper-body layers;
  - LODs.

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
- **MakeHuman's base mesh, the first version's body.** Its athletic recipe
  read as a nude human with a small head under flat-coloured materials.
  Blender Studio's base is quad topology built for sculpting, with UVs and
  sculpt face sets that locate the joints.

## Revisit when

- Projects need a character asset setting, or prefabs spawn models (the
  [behavior proposal](../proposals/entity-behavior-system.md)).
- Faster backward and strafe motion capture becomes available, or foot IK,
  turn-in-place or aim layers are added.
- A timing gate puts the mannequin's skinning cost against the frame budget.
- The mesh cooker can share one cooked image between occlusion and
  metallic-roughness, or packages need a smaller texture budget or LODs.

## Evidence

macOS 26.6.2, Apple M1 Pro, Release, Metal, Blender 5.2.2.

**The current content, 2026-10-01.** The working tree also held another
session's uncommitted atmosphere and IBL shader edits.

- **Sources.** `fetch_sources.py` into an empty directory downloaded and
  verified all 19 inputs and unpacked the Blender Studio archive; a second run
  downloaded nothing.
- **Build.** `build_mannequin.py --sources <that directory> --blend` took
  191 s at 5.3 GB peak memory and wrote:
  - a game mesh of 120,950 vertices, from a 3,237,650-vertex detail mesh;
  - 2048² textures.
- **Motion check.** `check_mannequin_motion.py` on that build passed all 18
  clips:
  - worst planted-foot slip: `Walk_Fwd`, p95 0.043 m/s (max 0.046); every
    other clip p95 at most 0.006 m/s;
  - loop seams at most 0.06°; lowest point −5.5e-7 m;
  - mean head pitch from −6.0° (crouching) to 0.0°;
  - torso lean from 0.6° (`Idle`) to 10.2° (`Run_Fwd`), and 19.6° to 26.0°
    crouching;
  - no hand inside the hips: the closest, in `Jog_Fwd`, stays at 1.12 times
    the hip volume's radius.
- **Builds.** These build with no compiler warnings:
  - `./build_editor.sh Release`;
  - `VKR_BUILD_TARGET=vkr_player ./build.sh Release`, whose template
    `engine/` holds the new files;
  - `VKR_BUILD_TARGET=vulkan_renderer_tester ./build.sh Release`.

  `build_release/tests/vulkan_renderer_tester`, whose asserts compile with
  `-UNDEBUG`, passes every suite:
  - `scene_animation_tests.c`: spawning refuses a wrapper without a transform
    and a second model on one wrapper. Despawning and destroying the wrapper
    both release the nodes and pool blocks. A bank cooked from another source
    fails and leaves nothing behind.
  - `player_animation_test.c` runs locomotion through a real player, with
    the regenerated clip table:
    - A loop at its authored speed plays at 1×.
    - Midway between walk and jog, the weights are 0.5 and 0.5 on one phase,
      which advances by the blended stride.
    - Past its loop speed, the run plays faster.
    - Sidestepping works, including the fallback for a missing loop.
    - Jump start, loop and land take the right shares, and no ground loop
      leaks into the landing.
    - A short hop lands without `Jump_Land`; crouching blends in.
    - The pose follows render time.
  - `script_host_test.c`: `renders_mesh` finds a descendant mesh and a shape,
    and returns false for an empty entity.
- **Bistro, headless editor.** A created Player Start spawned `Player` with
  the mannequin, from 5,990 to 6,073 entities, with no warning; Stop restored
  5,990.
- **Bistro capture.** `vkr_harness snapshot` of a local Bistro scene with the
  mannequin idling, profile `local-offscreen`, passed (report
  `sha256:59d68540b46eb6c38d2b842a18622c29027b5107b414f1eff138a55cb622b684`).
  The capture shows the textured shell.
- **Package.** `check_bakery_package.py` passes. Bundling its project wrote an
  `engine.vkpak` of 17 entries (42.0 MiB) with the eight mannequin entries,
  each named image as its `.vkt`.

**Spawning, body choice and locomotion, 2026-09-30.** This code is unchanged
since; the content then had the same 81 source nodes.

- Restoring the landing's double-counted air share fails the tester, and so
  does posing one tick ahead.
- **ASan.** The Debug (AddressSanitizer) tester passes every suite under
  `ASAN_OPTIONS=report_globals=0`. Without that option, the reload suite hits
  its known ASan global fault ([ADR-079](079-c-script-modules.md)).
- **Template project.** RPG Grounds from `check_default_templates.py`, in the
  headless editor:
  - Play went from 472 to 554 entities: `PlayerBody` and 81 source nodes.
  - Stop restored 472, with no warnings.
- **Bistro, headless.**
  - A cube with `fps_player` stayed its own body.
  - An empty entity with `fps_player` got a `PlayerBody` (+82 entities).

Not run:

- Windows and Vulkan;
- movement driven from the keyboard in a window (no synthetic input was
  sent);
- a packaged game's window with the current content;
- ASan on the current content, whose only C change is the generated clip
  table;
- timing.
