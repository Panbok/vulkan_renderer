---
status: implemented
updated: 2026-10-06
authority: adr
---

# ADR-090: Time of day

## Status

Accepted. The World clock, the turning sun and moon, the night fade, light
group factors in the runtime lighting, diffuse volumes that follow the sun and
the light groups, and the script and command controls are implemented. The
tiled pipeline scales its baked lamp layers ([ADR-088](088-baked-lightmap-sets.md))
by the same group factors
([ADR-087](087-gpu-class-graphics-pipelines.md), decision 8).

## Context

[ADR-087](087-gpu-class-graphics-pipelines.md) requires a day/night cycle, and
[ADR-088](088-baked-lightmap-sets.md) bakes the sun's bounce into eight sun
keys on its daily circle and static lamps into named lamp groups that the
runtime scales. The atmosphere already follows a moving sun light and carries
a moon ([ADR-058](058-revision-baked-sky-atmosphere.md),
[ADR-081](081-physical-night-sky.md)), but nothing turned the sun or switched
lamps.

Owner decisions (2026-10-05): one clock per world, authored as a World
component with an hour and a day length; the sun and moon turn about the
celestial pole from their authored directions, taken as noon; light groups
listed as night groups fade in as the sun sets and out at dawn, and scripts
and commands can set any group's intensity.

## Decision

### Clock

`time_of_day` is a World-only singleton component
([`vkr_scene_system.h`](../../runtime/src/renderer/systems/vkr_scene_system.h),
`SceneTimeOfDay`; descriptor in
[`vkr_scene_types.c`](../../runtime/src/renderer/systems/vkr_scene_types.c)):
`enabled`, `hour` in [0, 24], `day_minutes` (real minutes per day; zero keeps
the hour) and `night_groups`, light group names separated by commas. Every
scene resolves it from the root World. The Details panel shows the hour as a
slider; the editor's Environment menu adds it.

The hour of a frame derives from the scene's simulation clock
(`vkr_scene_sync_sun`): the authored hour, or an hour set through
`vkr_scene_set_time_of_day_hour` at the tick it was set, advanced by the
completed fixed ticks times 24 hours per `day_minutes` minutes. A paused
editor therefore shows the authored hour, a running game advances it, and a
simulation reset returns to the authored hour.

### Sun and moon

With the clock enabled, the resolved sun and moon light directions turn by
`vkr_scene_time_of_day_angle(hour)`, −(hour − 12) × 15 degrees, right-handed
about the celestial pole of the scene's atmosphere (the default pole without
one). The negative sense is the sky's: under the default pole the sun sets
toward +X. Hour 12 leaves the authored directions; authors set the sun to its
noon position. The direct light, its shadows, the drawn sky and the sky light
follow through the existing sun refresh. Lightmap sun key k
([ADR-088](088-baked-lightmap-sets.md)) lies at hour 12 − 3k.

### Night fade and light groups

The night fade is 1 − smoothstep of the sun's elevation from 0 to 5 degrees:
zero while the sun stands 5 degrees or more above the horizon, one once it has
set, and one without a sun.

Each scene registers the light groups its lights name, up to 16
(`VkrSceneLightGroups`). Setting a point or rectangle light validates its
mobility and group name and stores its group's slot in the component; a
dynamic light has none. Each frame a group's factor is its intensity (one
until `vkr_scene_set_light_group_intensity` sets it, restored by a simulation
reset) times the night fade when the time of day lists it as a night group.
The lighting system multiplies a static light's intensity or radiance by its
group's factor and skips a light whose factor is zero
([`vkr_lighting_system.c`](../../runtime/src/renderer/systems/vkr_lighting_system.c)).
Additive scenes use the rendered scene's night fade and night groups.

Baked diffuse volumes hold one layer per sun key and lamp group; the scene
recomposes the volume texture from the two sun keys nearest the current sun
and the lamp groups' factors ([ADR-054](054-baked-diffuse-volumes.md)).

### Control

Scripts read the hour with `vkr_time_of_day` and set it with
`vkr_set_time_of_day`; `vkr_set_light_group` sets a group's intensity in
every attached container ([`sdk.h`](../../sdk/sdk.h), SDK version 7,
[ADR-079](079-c-script-modules.md)). The editor commands `time.hour <hour>`
and `light.group <group> <intensity>` ([ADR-075](075-editor-cmd-bar-and-evaluator.md))
reach the runtime through the frame's time-of-day request: the hour goes to
the rendered scene and the intensity to the active scene, the World and the
added scenes. Both last until the simulation resets; the authored hour is the
component's property, which the Details slider and `vkr_component_set` edit.

## Consequences

- The desktop pipeline switches and dims static lamps at runtime, and the
  tiled pipeline scales their baked lamp layers by the same group factors
  (`vkr_scene_light_layer_weights`).
- Diffuse volumes take one SH set per probe per layer, about ten times the
  bake time of a single layer, and between two sun keys the blend
  approximates the bounce of a sun between them.
- Volumetric clouds shadow the sun as it turns: a level can fall into cloud
  shadow at some hours.
- A sun close to the celestial pole never sets, so its night groups never
  light.

## Alternatives considered

- **Per-scene clock in the atmosphere block.** Each scene would keep its own
  hour, and the World's sun could not be shared.
- **Scripts only.** Nothing to see in the editor until a script exists.
- **Per-group on and off hours.** More authoring for the common street-lamp
  case; scripts still cover schedules.

## Evidence

Release build, M1 Pro, 2026-10-05:

- `test_time_of_day_turns_sun_and_switches_night_groups` (`vulkan_renderer_tester`):
  noon keeps the authored sun and turns night groups off; 18:00 sets the sun
  toward +X under the default pole and lights them; group intensities scale
  static lights and leave dynamic ones; a script-set 06:00 raises the sun from
  −X and a 24-minute day advances it one hour per 3,600 ticks; a simulation
  reset restores the authored hour and intensities; an invalid night group
  list is rejected.
- The blockout of ADR-088 in the headless editor through `vkr_mcp`: a World
  `time_of_day` with night group `warm` and the scene atmosphere's
  `celestial_pole` set to (0, 0.8, −0.6) so that its sun sets. With the scene's
  clouds disabled, captures at 12:00 and 17:00 show the sun lowering and the
  shadows lengthening; at 21:00 the sky is dark and lamp group `warm` lights
  its room, which it leaves dark at noon.
- The same editor through `vkr_mcp`'s `vkr_cmd`: `time.hour 21` lit the night
  group, `light.group warm 0` switched it off and `light.group warm 0.25`
  dimmed it; `light.group "bad name" 1` and `time.hour x` were refused with
  their messages.

Unavailable: a Vulkan run, and a script module calling the SDK entries.

## Revisit when

A level needs more than 16 light groups, or sun-key blending shows between
keys.
