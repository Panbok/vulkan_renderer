---
status: proposed
updated: 2026-10-04
authority: proposal
---
# Level toolkit Windows/Vulkan handoff

Steps for a Windows host with a Vulkan 1.4 GPU (RDNA 2+ or Ampere+,
[ADR-083](../adr/083-supported-hardware-matrix.md)) to verify the level
toolkit, terrain, geometry LOD and world partition natively
([ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md),
[ADR-085](../adr/085-gpu-geometry-lod-and-terrain-geomorphing.md),
[ADR-086](../adr/086-world-partition.md)). Each step closes items of the
[level toolkit audit](level-toolkit-audit.md); record results in the
[Windows/Vulkan verification checklist](windows-vulkan-verification.md).

## Before you start

- Check out `main` at or after `afaac501` and recook as the checklist's
  2026-10-03 record did.
- Run one GPU process at a time. Use Bistro only.
- The agent socket is off on Windows (ADR-084), so these steps drive the
  editor with `--exec` scripts. The Cmd statement `op <operation> <json>`
  runs any agent operation in process; its result prints as an `[agent]`
  line and each statement as `[cmd]` lines.
- Each run creates `assets\terrain\<uuid>.vkrhf`. Delete only the files your
  runs created, and the `wp_test` scene copies, when you finish.

Use this PowerShell setup for every editor run. `$Exe` is the Release editor
for H4 to H8 and the Debug editor, which enables Vulkan validation with
synchronization checks, for H2 and H3.

```powershell
$Root = (Get-Location).Path
New-Item -ItemType Directory -Force "$Root\.scratch\wp\home" | Out-Null
$env:HOME = "$Root\.scratch\wp\home"
$env:VKR_EDITOR_LAYOUT_PATH = "$Root\.scratch\wp\layout.json"
$env:VKR_GRAPHICS_SETTINGS_PATH = "$Root\.scratch\wp\graphics.json"
$env:VKR_AUTOCLOSE_SECONDS = "600"
Remove-Item Env:VK_INSTANCE_LAYERS -ErrorAction SilentlyContinue
function Run-Editor($Exe, $Scene, $Script, $Log) {
  & $Exe --headless --renderer vulkan --scene $Scene --exec $Script *> $Log
  Select-String -Path $Log -Pattern '\[cmd\]|\[agent\]|VUID|error' |
    ForEach-Object { $_.Line }
}
```

Build the Release editor with `build_editor.bat` and the Debug editor with
`set VKR_BUILD_TARGET=vkr_editor` and `build.bat Debug`.

## Steps

### H1. CPU suites (audit A2)

Run `build_test.bat`. Every suite must pass; these cover the new code
directly: `heightfield` (tiled and streamed fields, in-place saves),
`scene_edit` (cell documents, unload rules), `io` (router refresh) and
`scene_physics` (rebase with exact restore).

### H2. Terrain layer blend (audit A2)

Debug editor, Bistro:

```powershell
Run-Editor $Exe assets\scenes\bistro.scene.json 'wait.scene; op terrain.create {"position":[0,-0.5,-720],"size":1024,"review":false}; op terrain.brush {"terrain":"Terrain","mode":"raise","points":[[-150,0,-500],[120,0,-650]],"radius":90,"strength":40,"review":false}; op terrain.brush {"terrain":"Terrain","mode":"paint","layer":3,"points":[[120,0,-650]],"radius":60,"strength":1,"review":false}; op view.camera {"eye":[0,45,-240],"target":[0,0,-700]}; wait 4; op view.capture {}' .scratch\wp\h2.log
```

Expected: Vulkan validation initializes, the log has no VUID or validation
error, and the capture shows two hills on the dev grid with one painted in the
fourth layer's blue, as on Metal.

### H3. LOD selection and geomorph (audit A2)

Debug editor; the H2 statements, then `view.mode wireframe; wait 1; op
view.capture {}`, then 20 moves from `"eye":[0,25,-300]` toward
`"eye":[0,25,-400]` with `"glide":true` and a capture at each end.

Expected: no validation error; the wireframe shows dense tiles near the camera
and sparser ones far away; the gliding captures show no popping at tile
level changes.

### H4. Streamed 8 km terrain (audit A2, A7, A9)

Release editor:

```powershell
Run-Editor $Exe assets\scenes\bistro.scene.json 'wait.scene; op view.camera {"eye":[0,60,-700],"target":[0,0,-1200],"far":9000}; op terrain.create {"position":[0,-0.5,-4700],"size":8192,"review":false}; op terrain.brush {"terrain":"Terrain","mode":"raise","points":[[-150,0,-1500],[0,0,-6000],[2000,0,-4000]],"radius":120,"strength":60,"review":false}; wait 8; op scene.describe {"region":{"min":[-5,-5,-4705],"max":[5,5,-4695]}}; op query.raycast {"origin":[0,200,-750],"direction":[0,-1,0],"distance":400}; op view.capture {}; view.mode wireframe; wait 1; op view.capture {}' .scratch\wp\h4.log
```

Expected: the terrain file is 409 MiB; `held` reports `"streamed":true`,
about 250 detail tiles, 64 overview tiles and 25 body tiles; the raycast hits
`Terrain`; the lit capture shows the far hills and the wireframe shows dense
tiles near the camera and coarse overview tiles beyond, with no gaps.

### H5. Proxies (audit A13)

Copy `assets\scenes\bistro.scene.json` to `assets\scenes\wp_test.scene.json`.
Release editor, scene `assets\scenes\wp_test.scene.json`:

```powershell
Run-Editor $Exe assets\scenes\wp_test.scene.json 'wait.scene; op entity.create {"name":"Partition","component":{"type":"world_partition","values":{"load_radius":256,"proxy_radius":3000}},"review":false}; op brush.box {"min":[580,0,-620],"max":[600,12,-600],"review":false}; op brush.box {"min":[610,0,-630],"max":[625,6,-615],"review":false}; scene.save' .scratch\wp\h5a.log
build_release\tools\bakery\vkr_bakery.exe bake proxies --scene assets\scenes\wp_test.scene.json
Run-Editor $Exe assets\scenes\wp_test.scene.json 'wait.scene; op view.camera {"eye":[600,120,-150],"target":[600,0,-600],"far":5000}; wait 4; op partition.describe {}; op view.capture {}; op view.camera {"eye":[600,60,-480],"target":[600,0,-600]}; wait 4; op partition.describe {}; op view.capture {}' .scratch\wp\h5b.log
```

Expected: `wp_test.scene.cells\4_-5.json` and `index.json` exist; the bake
prints `proxies: 1 cells, 1 cooked` and a second bake `0 cooked`; the first
describe reports cell `[4,-5]` with `"proxy":true` and the first capture shows
the proxy boxes; after the move the cell reports `"loaded":true` and the
second capture shows the brushes.

### H6. Origin rebase (audit A2, A8)

Release editor, Bistro:

```powershell
Run-Editor $Exe assets\scenes\bistro.scene.json 'wait.scene; op terrain.create {"position":[4000,-0.5,0],"size":4096,"review":false}; op view.camera {"eye":[4500,30,0],"target":[4500,10,-150],"far":3000}; wait 3; op scene.describe {"region":{"min":[-1,-1,-1],"max":[1,1,1]},"limit":1}; sim.play; wait 3; op scene.describe {"region":{"min":[-4097,-1,-1],"max":[-4095,1,1]},"limit":1}; op query.raycast {"origin":[404,200,-20],"direction":[0,-1,0],"distance":400}; op view.capture {}; sim.stop; wait 2; op scene.describe {"region":{"min":[-1,-1,-1],"max":[1,1,1]},"limit":1}' .scratch\wp\h6.log
```

Expected: the log prints `Origin rebased by 4096, 0 m`; during Play
`SceneRoot` is at `[-4096,0,0]` and the raycast hits `Terrain`; after
`sim.stop` it is at `[0,0,0]` exactly; the capture's clouds match the
pre-Play view.

### H7. Hitch measurement (audit A1)

Release editor, Bistro, `gfx.preset = "high"` as the first statement after
`wait.scene`. Run 240 moves of 3 m along -Z every 0.1 s from
`"eye":[0,60,-700]`, each `op view.camera {..., "glide":true}`, reading
`stats.frame_ms`, `stats.frame_ms_p95` and `stats.frame_ms_max` every 40
moves. Run it three times: without terrain, with the H4 terrain, and with the
H4 terrain and `"local_shadows": false` in `graphics.json`.

Expected on Metal for comparison (ADR-086): p95 6.4 ms without terrain, about
30 ms with it, about 10 ms with it and local shadows off. Record whether
Vulkan shows the same pattern; it decides whether A1 is shared code or
Metal-specific.

### H8. Large files (audit A3, A9)

Copy Bistro to `wp_test` as in H5. In one run on that scene, create the H4
terrain, raise it with `op terrain.brush` at `[0,0,-6000]` (60 m), read
`op terrain.sample {"terrain":"Terrain","points":[[0,0,-6000]]}` and
`scene.save`. In a second run on the same scene, read the sample again.

Expected: the second run reads the same raised height, which proves the
in-place tile write and the overview on Windows file I/O; the file stays
409 MiB. The file stays below 2 GiB, so offsets past 2 GiB remain untested.

### H9. Agent operations without the socket (audit A2)

Every step above runs operations through `op`. Confirm `[agent]` lines carry
`"ok":true` for each `op`, and note that the socket and `vkr_mcp` remain
unavailable on Windows (ADR-084).

## Record

For each step record the commit, GPU, driver, build type, the exact command,
the decisive log lines and capture paths. A step passes only when the log has
no validation error (Debug runs) and the expected values above hold.
