---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../../architecture/renderer-architecture-spec.md`](../../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
## Front-end API — Shader System

### Configuration
```c
typedef struct shader_system_config {
  uint16_t max_shader_count;       // >= 512 recommended
  uint8_t max_uniform_count;       // per-shader (all scopes combined)
  uint8_t max_global_textures;     // per-shader global scope
  uint8_t max_instance_textures;   // per-shader instance scope
} shader_system_config;

// Recommended defaults
#define SHADER_SYSTEM_CONFIG_DEFAULT { \
  .max_shader_count = 512,       \
  .max_uniform_count = 32,       \
  .max_global_textures = 8,      \
  .max_instance_textures = 8     \
}
```

**Rationale for Defaults**:
- **512 shaders**: Enough for a medium-large game (100+ materials × multiple variants)
- **32 uniforms**: Covers most shaders (PBR needs ~15: projection, view, model, albedo, normal, metallic, roughness, AO, emissive, etc.)
- **8 textures per scope**: Sufficient for PBR materials with extra slots for future expansion

### Shader handles and metadata
```c
typedef struct shader shader;            // Opaque to users
typedef struct shader_uniform shader_uniform;

typedef enum shader_scope { SHADER_SCOPE_GLOBAL=0, SHADER_SCOPE_INSTANCE=1, SHADER_SCOPE_LOCAL=2 } shader_scope;
typedef enum shader_stage { SHADER_STAGE_VERTEX=1, SHADER_STAGE_GEOMETRY=2, SHADER_STAGE_FRAGMENT=4, SHADER_STAGE_COMPUTE=8 } shader_stage;
```

### System API
```c
bool8_t shader_system_initialize(uint64_t *memory_requirement, void *memory, shader_system_config cfg);
void shader_system_shutdown(void *state);

bool8_t shader_system_create(const struct shader_config *cfg);
uint32_t shader_system_get_id(const char *shader_name);
shader *shader_system_get_by_id(uint32_t shader_id);
shader *shader_system_get(const char *shader_name);

bool8_t shader_system_use(const char *shader_name);
bool8_t shader_system_use_by_id(uint32_t shader_id);

uint16_t shader_system_uniform_index(shader *s, const char *uniform_name);
bool8_t shader_system_uniform_set(const char *uniform_name, const void *value);
bool8_t shader_system_sampler_set(const char *sampler_name, const struct texture *t);

bool8_t shader_system_uniform_set_by_index(uint16_t index, const void *value);
bool8_t shader_system_sampler_set_by_index(uint16_t index, const struct texture *t);

bool8_t shader_system_apply_global(void);
bool8_t shader_system_apply_instance(void);
bool8_t shader_system_bind_instance(uint32_t instance_id);
```

### Instance resource management
```c
bool8_t shader_acquire_instance_resources(shader *s, uint32_t *out_instance_id);
bool8_t shader_release_instance_resources(shader *s, uint32_t instance_id);
```

### Error handling
- Return `false` on failure; log with context and shader name.
- Uniform lookups by name return `INVALID_ID_U16` if not found.
- Invalid shader IDs log error and return early (no crash).
- Setting uniforms on wrong scope logs warning but doesn't fail (allows graceful degradation).

**Example Error Messages**:
```c
log_error("Shader '%s': uniform 'invalid_name' not found", shader->name);
log_error("Shader '%s': failed to apply global uniforms", shader->name);
log_warn("Shader '%s': uniform 'diffuse_color' set before binding instance", shader->name);
log_error("Shader system: max shader count (%u) reached", config.max_shader_count);
```

### Usage sketch
```c
// Load config → create shader
struct shader_config cfg = load_shader_cfg_from_file(path);
shader_system_create(&cfg);

// Use shader
shader_system_use("shader.default.world");

// Global per-frame
shader_system_uniform_set("projection", &proj);
shader_system_uniform_set("view", &view);
shader_system_apply_global();

// Per-material instance
uint32_t instance_id; shader_acquire_instance_resources(shader_system_get("shader.default.world"), &instance_id);
shader_system_bind_instance(instance_id);
shader_system_uniform_set("diffuse_color", &color);
shader_system_sampler_set("diffuse_texture", texture_ptr);
shader_system_apply_instance();
```


