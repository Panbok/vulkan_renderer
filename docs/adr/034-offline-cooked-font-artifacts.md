---
status: implemented
updated: 2026-09-06
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

The repository ships cooked regular and bold Ubuntu Mono bootstrap atlases, plus
the small bitmap compatibility atlas required during font-system startup. Normal
app/editor build wrappers compile cooker binaries without baking these assets.

The regular and bold atlases use 64 atlas texels per em and a 16-texel distance
range. This supplies a screen range of two at 8 physical pixels per em; the old
8-texel setting dropped below two for small UI text on a 1x display. Atlas
dimensions remain 1024 by 1024, with unchanged glyph advances and font metrics.
The wider field expands glyph quads, so unchanged atlas storage does not imply
unchanged fragment work.

The editor Bakery invokes the same incremental font cooker for later changes;
headless tooling can still call it directly. This keeps first launch usable before
Bakery runs and preserves the offline runtime font contract.

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
