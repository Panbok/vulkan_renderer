---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../../architecture/renderer-architecture-spec.md`](../../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
## Validation & Testing

### Unit tests
- Config parser:
  - Valid/invalid keys
  - Stage alignment (stages ↔ files)
  - Attribute/uniform parsing
- Offset/stride math:
  - Aligned stride computation
  - Push constant alignment
- Name lookup:
  - Uniform name → index; sampler slot ordering

### Integration tests
- Create pipelines from UI/world shader configs; render a frame without errors.
- Update global (view/projection) and instance (diffuse color + texture); verify via readbacks/screenshots.

### Regression tests (Critical for Migration)
Ensure new config-driven system produces **identical rendering** to old hardcoded system:

```c
void test_world_shader_visual_regression() {
  // Setup: create identical scene with same geometry/materials/camera

  // 1. Render frame with OLD hardcoded shader system
  uint8_t *old_framebuffer = render_frame_old_system();

  // 2. Render frame with NEW config-driven shader
  //    (using .shadercfg that matches old hardcoded values)
  uint8_t *new_framebuffer = render_frame_new_system("shader.default.world");

  // 3. Compare framebuffer bytes (should be IDENTICAL)
  bool identical = memcmp(old_framebuffer, new_framebuffer,
                          width * height * 4) == 0;
  assert_log(identical, "Visual regression detected!");

  // 4. If not identical, save diff image for debugging
  if (!identical) {
    save_diff_image("regression_diff.png", old_framebuffer, new_framebuffer);
  }
}

void test_ui_shader_visual_regression() {
  // Same as above but for UI domain
}
```

**Rationale**: Catches subtle bugs in UBO packing, descriptor binding, or uniform uploads.

### Stress tests
```c
void test_max_shaders() {
  // Create max_shader_count shaders (512)
  for (u32 i = 0; i < 512; i++) {
    char name[64];
    snprintf(name, sizeof(name), "shader.test_%u", i);

    shader_config cfg = create_minimal_config(name);
    bool ok = shader_system_create(&cfg);
    assert_log(ok, "Failed to create shader %u", i);
  }

  // Ensure all are usable
  shader *s = shader_system_get("shader.test_0");
  assert_log(s != NULL, "Failed to retrieve shader");
}

void test_max_uniforms() {
  // Create shader with 32 uniforms (max)
  shader_config cfg = create_config_with_n_uniforms(32);
  shader_system_create(&cfg);

  shader *s = shader_system_get(cfg.name);

  // Set all uniforms and render
  for (u32 i = 0; i < 32; i++) {
    char uniform_name[64];
    snprintf(uniform_name, sizeof(uniform_name), "uniform_%u", i);

    Vec4 value = vec4_new(1, 2, 3, 4);
    shader_system_uniform_set(uniform_name, &value);
  }

  shader_system_apply_global();
  shader_system_apply_instance();

  // Render and verify no crashes/errors
  render_test_frame();
}

void test_max_textures() {
  // Create shader with 8 instance textures
  shader_config cfg = create_config_with_n_textures(8);
  shader_system_create(&cfg);

  // Load 8 unique textures
  texture *textures[8];
  for (u32 i = 0; i < 8; i++) {
    textures[i] = load_texture("test_texture.png");
  }

  // Bind all textures
  shader_system_use(cfg.name);
  shader_system_bind_instance(0);
  for (u32 i = 0; i < 8; i++) {
    char sampler_name[64];
    snprintf(sampler_name, sizeof(sampler_name), "texture_%u", i);
    shader_system_sampler_set(sampler_name, textures[i]);
  }
  shader_system_apply_instance();

  render_test_frame();
}

void test_ubo_alignment_edge_cases() {
  // Test vec3 alignment (should waste 4 bytes)
  shader_config cfg = {
    .uniforms = {
      { .type = SHADER_UNIFORM_TYPE_FLOAT32_3, .scope = SHADER_SCOPE_GLOBAL },
      { .type = SHADER_UNIFORM_TYPE_FLOAT32,   .scope = SHADER_SCOPE_GLOBAL },
    }
  };

  // Expected layout:
  // vec3 at offset 0, size 12, next aligned to 16
  // float at offset 16, size 4
  // Total: 20 bytes, stride: 256

  shader_system_create(&cfg);
  shader *s = shader_system_get(cfg.name);

  assert_log(s->global_ubo_size == 20, "Unexpected UBO size");
  assert_log(s->global_ubo_stride == 256, "Unexpected UBO stride");
}
```

### Runtime assertions
- Descriptor set presence before binding.
- Size bounds on buffer loads.
- Push constant size ≤ device limit.

### Telemetry & stats
- Use existing pipeline registry frame stats; add shader-system counters:
  - global/instance applies per frame
  - descriptor writes avoided
  - instance resources acquired/released


