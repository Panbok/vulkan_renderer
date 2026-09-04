---
status: implemented
updated: 2026-09-05
authority: adr
---
# ADR-012: KTX2/UASTC texture artifacts with capability-selected transcode

## Status

Accepted.

## Context

Texture source images are unsuitable as the normal runtime distribution format,
but the selected GPU compression family varies by device.

## Decision

The texture packer writes KTX2 containers with Basis UASTC payloads using the
runtime `.vkt` extension. Runtime detection uses the container signature, not
the extension alone. The loader selects a transcode target from texture class,
sRGB intent, device class, and advertised BC7, BC5, ASTC, ETC2, and EAC RG11
support. Every selected target has a libktx transcode mapping; RGBA32 is the
terminal fallback.

The runtime accepts 2D, array, cubemap, and cubemap-array KTX2 payloads. It
caches target-native transcode output. Strict `.vkt` mode disables source and
legacy-raw fallback; development configuration retains those migration paths.

## Consequences

Texture class is part of the asset contract: color, normal, and data textures
cannot share an arbitrary transcode policy. Writable and resize paths must
reject block-compressed textures. A legacy raw `.vkt` is migration support, not
the canonical artifact.

## Alternatives considered

Shipping one pre-transcoded format requires separate asset sets. Loading only
raw source images shifts decode and bandwidth costs into runtime loading.

## Revisit when

The supported device profile adds a compression family or the runtime no longer
needs legacy `.vkt` compatibility.

## Code evidence

- [packer](../../tools/vkr_vkt_packer.cpp)
- [selection and KTX2 load](../../lib/src/renderer/systems/vkr_texture_system.c)
- [texture contract](../../lib/src/renderer/systems/vkr_texture_system.h)
