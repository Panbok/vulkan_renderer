---
status: superseded
updated: 2026-07-31
authority: design
---

> **Archived.** Superseded by [`../../../architecture/renderer-architecture-spec.md`](../../../architecture/renderer-architecture-spec.md). Retained for history; do not treat as current.
# Rendering Errors - FIXED ✅

## Issues Found and Fixed

### Problem 1: Pipeline Aliasing Bug
**Root Cause**: The shader loader was creating non-null-terminated strings for shader names, causing `string8_create_formatted` to read beyond the string boundary when using `%s` formatting.

**Fix Applied** (`shader_loader.c` lines 286-290, 294-298):
- Added arena-allocated null-terminated copies of shader names and renderpass names
- Ensures safe string formatting operations downstream

**Result**: Pipelines are now correctly aliased with keys like `"shader.default.world_0"` instead of garbage text.

---

### Problem 2: Uniform Names Include Comments
**Root Cause**: The shader config parser was storing entire uniform declaration lines (including comments) as uniform names. For example: `"projection    # Camera projection matrix"` instead of just `"projection"`.

**Fix Applied** (`shader_loader.c` lines 394-405, 375-381):
- Modified uniform/attribute parsing to stop at `#` character (comment delimiter)
- Added whitespace trimming to remove trailing spaces
- Applied to both uniform and attribute name parsing

**Result**: Uniform lookups now work correctly - all uniforms are found and bound properly.

---

## Validation

### Before Fixes:
```
❌ Duplicate pipelines created every frame
❌ "uniform 'projection' not found" warnings
❌ "uniform 'model' not found" warnings
❌ "sampler 'diffuse_texture' not found" warnings
❌ Validation layer errors about unbound descriptors
```

### After Fixes:
```
✅ Single pipeline created per shader, reused correctly
✅ All uniforms found and validated
✅ Zero "uniform not found" warnings
✅ Zero validation layer errors
✅ Application runs at ~60 FPS without issues
```

---

## Files Modified

1. **`lib/src/renderer/resources/loaders/shader_loader.c`**
   - Fixed shader name/renderpass name parsing (null termination)
   - Fixed uniform name parsing (comment stripping)
   - Fixed attribute name parsing (comment stripping)

2. **`lib/src/renderer/systems/vkr_pipeline_registry.c`**
   - Removed temporary debug logging

---

## Testing

Run the application to verify:
```bash
./build_run.sh
```

Expected result:
- Window opens
- Rendering works correctly
- Zero errors or warnings in logs
- Smooth ~60 FPS performance

