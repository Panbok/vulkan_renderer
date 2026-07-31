---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../../architecture/renderer-architecture-spec.md`](../../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
## Materials ↔ Shaders — Integration

### Goals
- Allow materials to choose a shader by name, or default by domain.
- Map material parameters to instance-scope uniforms and samplers.

### Material file support
- Optional: `shader=shader.default.world`
- Keep `pipeline=world|ui` for backward compatibility; if both are present, `shader=` wins.

### Loader mapping
- `diffuse_color` → instance UBO vec4 uniform.
- `base_color` → sampler slot 0 (enable flag set when present).
- Additional PBR keys can be mapped later to extra uniforms/samplers.

### Application draw path
1) Resolve shader: `shader_system_use(material.shader_name or default_for_domain)`.
2) Global: set projection/view via `shader_system_uniform_set` + `shader_system_apply_global`.
3) Instance: acquire resources if needed, bind instance; set `diffuse_color`, set sampler to texture0; `shader_system_apply_instance`.
4) Local: push model matrix via backend (`vkCmdPushConstants`).

### Compatibility
- If `shader=` is absent, existing pipeline registry code chooses a pipeline; shader system uses the default shader associated with that domain.

### Default Shader Mapping by Domain
```c
// In material_system.c or pipeline_registry.c
const char* get_default_shader_for_domain(VkrPipelineDomain domain) {
  switch(domain) {
    case VKR_PIPELINE_DOMAIN_WORLD:  return "shader.default.world";
    case VKR_PIPELINE_DOMAIN_UI:     return "shader.default.ui";
    case VKR_PIPELINE_DOMAIN_SHADOW: return "shader.default.shadow";
    case VKR_PIPELINE_DOMAIN_POST:   return "shader.default.post";
    default:
      log_warn("Unknown domain %u, using world shader", domain);
      return "shader.default.world";
  }
}

// Usage in material loader:
if (!material_config.shader_name) {
  VkrPipelineDomain domain = get_domain_from_pipeline_id(material_config.pipeline_id);
  material->shader_name = get_default_shader_for_domain(domain);
}
```

### Material File Extension
Add optional `shader=` field to `.mt` files:
```
# assets/materials/brick.mt
version=1.0
name=material.brick
pipeline=world              # Legacy, maps to VKR_PIPELINE_DOMAIN_WORLD
shader=shader.default.world # New, explicit shader selection (overrides pipeline)

diffuse_color=0.8,0.5,0.3,1.0
base_color=assets/textures/brick_albedo.png
normal=assets/textures/brick_normal.png  # Future: extra texture slots
```

**Backward Compatibility**:
- Old `.mt` files without `shader=` continue to work
- `pipeline=` field determines default shader via domain mapping
- New `.mt` files can use `shader=` for explicit control


