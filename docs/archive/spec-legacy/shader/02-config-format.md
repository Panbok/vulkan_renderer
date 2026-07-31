---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../../architecture/renderer-architecture-spec.md`](../../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
## Shader Config Format — `.shadercfg`

### Overview
Text-based key/value format. Loader is permissive about whitespace. Unknown keys are ignored with a warning.

### Top-level keys
- `version`: string, currently "1.0".
- `name`: unique shader name (used by lookup).
- `renderpass`: name of renderpass (for domain binding and compatibility checks).
- `stages`: comma-separated list: `vertex,fragment` (future: `geometry,compute`).
- `stagefiles`: comma-separated list of stage file paths, one per stage, or a single path for single-file multi-entry.
- `use_instance`: `0|1` - enables instance-scope UBO/samplers.
- `use_local`: `0|1` - enables push constants (model).

### Attributes
- Repeated `attribute=type,name`
- Types: `vec2, vec3, vec4, mat4, int8, uint8, int16, uint16, int32, uint32`.
- Location indices assigned in declaration order starting at 0.

### Uniforms
- Repeated `uniform=type,scope,name`
- Types: `float, vec2, vec3, vec4, int8, uint8, int16, uint16, int32, uint32, mat4, samp`.
- Scope: `0=global, 1=instance, 2=local` (local means push constant).
- Location/index assigned in declaration order within each scope.
- Samplers (`samp`) live in instance scope and map to texture slots in the order declared.

### Sizes & packing
- Scalars/vectors/matrices sizes follow current C structs used by the backend.
- Global/instance uniforms follow **std140 layout rules** (see below for alignment details).
- UBO stride = `align_up(ubo_size, required_ubo_alignment)` where alignment is device-specific (typically 256 bytes).
- Push constants aligned to 4 bytes.

#### std140 Alignment Rules
Each uniform member has specific alignment requirements:
- `float`, `int`, `uint`: 4-byte aligned
- `vec2`: 8-byte aligned
- `vec3`: **16-byte aligned** (wastes 4 bytes after the 12-byte value)
- `vec4`: 16-byte aligned
- `mat4`: treated as 4 consecutive `vec4`s, each 16-byte aligned (total 64 bytes)

Example calculation:
```
uniform=mat4,0,projection  → offset=0,   size=64
uniform=mat4,0,view        → offset=64,  size=64
uniform=vec4,0,time_data   → offset=128, size=16
→ global_ubo_size = 144
→ global_ubo_stride = align_up(144, 256) = 256  (device alignment)
```

#### Sampler Handling
- Samplers (`uniform=samp,scope,name`) do **not** occupy UBO space.
- Instead, they allocate descriptor bindings and a texture slot index.
- `shader_uniform.location` = texture slot index (0, 1, 2...) within the scope.
- `shader_uniform.size` = 0.
- Current descriptor layout uses separate image+sampler bindings (2 per texture).

Example:
```
uniform=vec4,1,diffuse_color     → instance UBO offset=0, size=16
uniform=samp,1,diffuse_texture   → texture slot 0, no UBO space
uniform=samp,1,normal_texture    → texture slot 1, no UBO space
→ instance_ubo_size = 16
→ instance_ubo_stride = align_up(16, 256) = 256
→ instance_texture_count = 2
```

### Stage packaging
- Single-file with multiple entry points:
  - `stages=vertex,fragment`
  - `stagefiles=assets/default.world.spv`
  - Entry points default to `vertexMain`/`fragmentMain`.
- Multi-file per stage:
  - `stagefiles=assets/default.world.vert.spv,assets/default.world.frag.spv`

### Example — World
```
version=1.0
name=shader.default.world
renderpass=renderpass.default.world
stages=vertex,fragment
stagefiles=assets/shaders/default.world.vert.spv,assets/shaders/default.world.frag.spv
use_instance=1
use_local=1

# Attributes : type,name
# Tightly packed in declaration order
attribute=vec3,in_position   # location=0, offset=0,  size=12
attribute=vec2,in_texcoord   # location=1, offset=12, size=8
# → attribute_stride = 20 bytes

# Uniforms : type,scope,name (0=global,1=instance,2=local)
# Global scope (set 0)
uniform=mat4,0,projection    # offset=0,  size=64
uniform=mat4,0,view          # offset=64, size=64
# → global_ubo_size=128, stride=align_up(128, 256)=256

# Instance scope (set 1)
uniform=vec4,1,diffuse_color    # offset=0, size=16
uniform=samp,1,diffuse_texture  # texture slot 0, no UBO
# → instance_ubo_size=16, stride=256, instance_texture_count=1

# Local scope (push constants)
uniform=mat4,2,model         # offset=0, size=64
# → push_constant_size=64, stride=64
```

### Example — UI
```
version=1.0
name=shader.default.ui
renderpass=renderpass.default.ui
stages=vertex,fragment
stagefiles=assets/shaders/default.ui.vert.spv,assets/shaders/default.ui.frag.spv
use_instance=1
use_local=1

# Attributes : type,name
attribute=vec2,in_position   # location=0, offset=0, size=8
attribute=vec2,in_texcoord   # location=1, offset=8, size=8
# → attribute_stride = 16 bytes

# Uniforms : type,scope,name (0=global,1=instance,2=local)
uniform=mat4,0,projection
uniform=mat4,0,view
uniform=vec4,1,diffuse_color
uniform=samp,1,diffuse_texture
uniform=mat4,2,model
```

### Example — Solid Color (No Textures)
```
version=1.0
name=shader.debug.solid_color
renderpass=renderpass.default.world
stages=vertex,fragment
stagefiles=assets/shaders/debug.solid_color.spv
use_instance=1
use_local=1

# Attributes
attribute=vec3,in_position
attribute=vec3,in_normal

# Uniforms (no samplers!)
uniform=mat4,0,projection
uniform=mat4,0,view
uniform=vec4,1,color        # Single solid color
uniform=mat4,2,model
```


