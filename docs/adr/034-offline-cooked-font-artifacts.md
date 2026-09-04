---
status: implemented
updated: 2026-09-05
authority: adr
---
# ADR-034: Cooked MTSDF font artifacts

## Status

Accepted.

## Context

Runtime font loading needs reproducible atlas data, float em metrics, glyph-ID
lookup, and an atlas contract that text preparation can consume directly.

## Decision

The default MTSDF font is a cooked VKFA v1 artifact. The format serializes
fields explicitly and validates its header, sections, bounds, overlap, and
checksums before exposing data. A cooked artifact supplies face metrics,
glyph-ID records, codepoint mapping, glyph-ID kerning, a fallback glyph, and a
linear RGBA8 MTSDF atlas. The cooked loader creates the font tables and owns the
atlas texture reference for the font lifetime.

Text layout resolves codepoints to glyph IDs, then uses glyph-ID kerning and
float em values. Bitmap and system-font loaders remain separate compatibility
paths; they do not redefine the cooked MTSDF contract.

## Consequences

The cooker and loader must advance together when VKFA changes. A malformed or
semantically incompatible artifact is rejected before publication. The atlas
must retain the field kind and sampling preconditions required by ADR-035.

## Alternatives considered

Runtime atlas generation makes asset identity and startup work host-dependent.
JSON metadata and loose image files leave validation and lifetime boundaries
split across unrelated loaders.

## Revisit when

Multi-page fonts, shaping beyond the current layout system, or font effects
require an expanded artifact contract.

## Code evidence

- [VKFA format](../../lib/src/renderer/resources/loaders/vkr_font_cooked.h)
- [cooked loader](../../lib/src/renderer/resources/loaders/cooked_font_loader.c)
- [font ownership](../../lib/src/renderer/systems/vkr_font_system.c)
- [layout lookup and kerning](../../lib/src/core/vkr_text.c)
