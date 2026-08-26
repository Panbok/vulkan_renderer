---
status: proposed
updated: 2026-08-26
authority: adr
---

# ADR-034: Offline-cooked font artifacts

## Status

Proposed. No production code implements this decision.

## Context

VKR carries bitmap, system-raster, and MTSDF font paths with different delivery
formats and metric conventions. The MTSDF path consumes a checked-in
msdf-atlas-gen JSON file plus a sibling PNG, named from a `.fontcfg` query such
as `file.json?atlas=...&size=18`. The repository contains no tracked producer,
recipe, or generator provenance for those files.

The current artifacts do not provide enough evidence to reconstruct their
inputs. `assets/fonts/Ubuntu-2d.json` has an empty kerning array, but an empty
result does not distinguish a face with no selected kerning pairs from a cook
that disabled kerning. Without the invocation and tool version, it cannot be
attributed to a particular option. The exact source face, character set,
variable-font axes, edge-coloring settings, and generator version are also
unknown.

The runtime then derives or discards information that should have been fixed by
the producer:

- `vkr_json_find_field()` tracks no object depth. A lookup for `atlas.emSize`,
  which msdf-atlas-gen does not emit, escapes the object and returns
  `metrics.emSize`. The current file loads only because that value is `1`.
- Half-texel `atlasBounds` are truncated into integers, biasing UV rectangles.
- The atlas PNG lives under `assets/textures/`, so the texture packer creates an
  sRGB, block-compressed, mipmapped `.vkt` sidecar. sRGB decoding is invalid for
  signed-distance values. Lossy block compression and box-filtered mips are
  outside the reference MTSDF sampling contract and can perturb channel order
  and encoded distance.
- The loader decodes a second CPU atlas copy into `VkrFont::atlas_cpu_data`,
  which no current consumer reads. The 2048-square atlas exceeds the font
  pool's 6 MiB chunk and logs an allocation failure during startup.
- Glyph lookup uses formatted decimal strings in the UI geometry path. Kerning
  is not loaded by the MTSDF path.

ADR-012 and ADR-030 establish the relevant pattern: version the runtime
artifact, pin the producer dependency, keep production tooling out of the
runtime, validate at the load boundary, and skip content-identical work. A font
cooker applies that pattern to fonts. It does not require font cooking to copy
the mesh cooker's wrapper policy exactly.

The comparison implementation `nuri` demonstrates that linking
msdf-atlas-gen into an offline tool is viable. Its representation is useful
evidence, but its names and scaling formula are not adopted blindly. The values
returned by msdf-atlas-gen's `GlyphGeometry` and `FontMetrics` are in em units;
they are not raw TrueType font units.

## Decision

Add a pinned `vendor/msdf-atlas-gen` dependency and a new offline
`vkr_font_cooker` tool. The tool emits a versioned `.vkfa` artifact. `.vkf`
remains reserved for the existing BMFont cache whose magic is `VKFT`.

### Tool and reproducibility

`vkr_font_cooker` is a C++ tool target. msdf-atlas-gen, msdfgen, and FreeType
are linked only into that target. Runtime targets receive no generator headers
or C++ dependency. Configure performs no network download and reports a missing
submodule with the repository's normal initialization instruction.

Each `.fontcfg` used by the cooker declares at least:

- source font path and face or collection index;
- explicit codepoint ranges or charset file;
- variable-font axis values when applicable;
- MTSDF field type, atlas pixels per em, distance range, padding, dimensions,
  and pixel format;
- edge-coloring algorithm and deterministic seed;
- fallback codepoint or glyph policy.

The artifact identity covers the source font bytes, face index, charset,
variable axes, every cooker setting, `.vkfa` format version, cooker version,
and pinned msdf-atlas-gen commit. Equal identities produce byte-identical
artifacts. Outputs are skipped when the identity and expected bytes are
unchanged, with a force option for diagnostics.

Root build integration, if selected during F1, must run only after the cooker
target builds successfully, publish the resulting artifact into the runtime
asset staging tree, and treat cooking or staging failure as build failure.
Standalone `.sh` and `.bat` wrappers remain available. Generated `.vkfa` files
are ignored build products; `.fontcfg` files and licensed source fonts are the
committed inputs.

### Semantic format contract

Version 1 is little-endian and serializes each scalar explicitly. It never
writes a C or C++ structure image, so compiler padding and host ABI do not enter
the file format. F1 owns a format header that freezes exact field widths,
offsets, alignment, and section ordering before a production artifact ships.

The semantic contract requires:

- a header with `VKFA` magic, format version, declared file size, flags, field
  kind, fallback glyph ID, counts, and a bounded section directory;
- a source-and-settings SHA-256 identity and checksums for independently read
  payload sections;
- font metrics, glyph plane bounds, advances, and kerning amounts as finite
  `float32_t` em values where one em is `1.0`;
- optional source `unitsPerEm` as provenance only, never as a divisor for the
  em-normalized records;
- a glyph table sorted by unique glyph ID, containing plane bounds, advance,
  normalized UV bounds with the vertical convention already applied, and a
  page index;
- a sorted, unique codepoint-to-glyph-ID map and a sorted, unique
  glyph-ID-pair kerning table;
- symmetric MTSDF distance range in atlas pixels and atlas pixels per em as
  separate quality metadata;
- a page directory with width, height, pixel format, row stride, payload
  offset, payload size, and checksum for each embedded page.

Version 1 production assets use one MTSDF page. The directory may describe
multiple pages so the file need not be redesigned, but the initial runtime
loader rejects `page_count != 1`. Supporting multiple pages requires an
explicit draw-partition or array-texture design; the current prepared-text draw
binds one atlas. The cooker also rejects asymmetric lower and upper distance
ranges until a format version records all required values.

The runtime scale from em metrics to device pixels is:

```text
device_value = em_value * requested_device_pixels_per_em
```

It is not `pxSize / sourceUnitsPerEm`. That formula is correct only for records
stored in raw source font units, which `.vkfa` does not store.

### Atlas format

The version 1 baseline is `R8G8B8A8_UNORM`, linear, uncompressed, and one mip
level. RGBA16F is an allowed cooker option only after a same-glyph A/B capture
shows a material benefit and both backend format/filter paths are validated.
Sixteen-bit storage doubles page residency and does not repair an incorrect
colorspace, compression mode, mip chain, or shader.

Font atlas pages do not enter the generic texture packer's input set. During
the loose-artifact transition, deleting stale `.vkt` sidecars and excluding
font atlases from packing are part of one change; adding a request query alone
is insufficient because a stale sidecar can still be selected.

### Loader boundary and ownership

A C11 resource loader decodes `.vkfa` through the existing
`VkrResourceLoader` vtable. Before publishing a GPU atlas or font handle it
validates:

- magic, supported version and field kind, declared file size, integer
  overflow, section bounds, alignment, non-overlap, and absence of unexplained
  trailing data;
- checksums, finite float values, sorted uniqueness, valid fallback and page
  references, normalized ordered UVs, supported pixel format, exact row and
  payload sizes, and supported distance-range semantics;
- that version 1 has exactly one page and that all map and kerning glyph IDs
  exist.

Temporary file and decode bytes live only through the cold load/finalize
boundary. Failure uses one cleanup path and releases any partially created GPU
resource. Published atlas ownership follows the existing resource/finalization
lifetime; no per-glyph allocation, formatting, I/O, or validation enters
layout or draw preparation.

The JSON-plus-PNG loader remains registered only through asset migration. It is
retired after the default UI and required world-text assets load from `.vkfa`
and rollback evidence has been recorded.

## Consequences

Font generation becomes reproducible and auditable. The runtime stops parsing
generator JSON, inferring coordinate conventions, truncating atlas bounds, and
decoding a write-only CPU atlas copy. Float em metrics support layout at any
requested device-pixel size without a baked-size quantization step.

The container preserves glyph IDs separately from codepoints. That supports
direct integer lookup now and avoids a format break if shaping is added later;
this decision does not add shaping.

The initial one-page restriction is deliberate. The current production range
U+0020 through U+00FF fits easily, while a complete CJK artifact introduces
coverage, atlas residency, and multi-page draw questions that this ADR does not
answer. Production `.fontcfg` coverage must be explicit and evidence-backed.

A new offline C++ dependency and cook step increase toolchain complexity.
Content hashing prevents unchanged fonts from being regenerated. The evidence
plan measures cook time, artifact size, peak decode bytes, and GPU resident
bytes separately; none is inferred from file format alone.

## Alternatives Considered

**Keep JSON plus PNG.** This is an acceptable short-lived F0 path after fixing
linear sampling, packer exclusion, range metadata, and float UV handling. It
does not capture producer identity or provide a validated single-file runtime
contract, so it is not the destination.

**Shell out to an installed msdf-atlas-gen CLI.** This avoids a submodule, but
makes the exact generator an environment dependency and cannot directly emit
the validated container. Rejected.

**Generate and cache glyphs at runtime.** This supports dynamic coverage but
adds unbounded first-use work and generator dependencies to the runtime.
Rejected for the renderer's current text use.

**Store a `.vkt` sibling.** This reuses texture delivery but restores a
two-file identity and requires overriding most generic texture policies.
Rejected until measured residency or streaming requirements justify it.

**Make RGBA16F the default.** This reduces distance quantization at twice the
RGBA8 residency. Rejected without visual evidence that quantization, rather
than the current colorspace, compression, range, and metric defects, is the
limiter.

**Extend `.vkf`.** The bitmap loader already claims that extension and the two
formats share neither field semantics nor lifetime requirements. Rejected.

## Revisit When

- A measured font-residency or streaming report requires compressed or
  separately streamable atlas pages.
- A required coverage set exceeds one page and a draw partition or texture
  array design has been accepted.
- Shaping is adopted and the cooker must preserve additional OpenType data.
- A pinned generator update changes output or API compatibility.
