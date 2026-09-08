---
status: implemented
updated: 2026-09-08
authority: adr
---

# ADR-061: Optional extended-linear display output

## Status

Accepted. Both native implementations are integrated. Metal output and lifecycle
checks pass; native Windows/Vulkan execution remains unavailable on this host.

## Context

The renderer preserves scene-linear HDR through lighting and reconstruction,
but its SDR display transform and 8-bit presentation attachments cannot expose
display headroom. An FP16 attachment alone does not extend the transform.

## Decision

Keep SDR as the default. `VKR_DISPLAY_OUTPUT_AUTO_EXTENDED_LINEAR` requests
extended-linear sRGB output when both the OS display provider and native
surface support it. Unsupported or unavailable capability uses the existing
SDR presentation path. Offscreen targets remain SDR. Runtime configuration and
`VKR_DISPLAY_OUTPUT=sdr|auto_extended_linear` expose the request; harness
manifests use `renderer.display_output` and override inherited output settings.

The window pump publishes a display snapshot: capability, current headroom relative to
SDR white, conversion to native output units, and a revision. The native
surface borrows its query callback and context until renderer destruction.
The render callback copies cached values; it does not call window-system APIs.
Windows defers DXGI queries until AUTO requests a snapshot, and its first frame
can use SDR until the pump publishes capability. Metal uses display-relative EDR units. Windows uses the current SDR white
setting divided by 80 nits as its scRGB output scale, and conservatively limits
relative headroom to full-frame luminance divided by SDR white. These units
follow [Microsoft's SDR white definition](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_sdr_white_level)
and [DXGI's full-frame luminance definition](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_6/ns-dxgi1_6-dxgi_output_desc1).

Capability and current range are separate. An EDR-capable Metal display may
report current headroom 1 before a layer requests extended range. FP16 activation
is allowed then, while the shader preserves the SDR curve until current
headroom rises. Potential headroom never substitutes for current shader limits.

After exposure and grading, retain the selected AgX/ACES result through scene
luminance 1. Above that anchor, raise highlights using a smooth shoulder whose
remaining distance to current headroom halves every four stops. Apply one RGB
scale to retain hue, bounded by the largest component. FXAA and sharpening
operate in display-relative units after this lift. Convert to native output
units once at final output. UI white retains its SDR luminance; editor
recomposition does not scale an already converted Scene image again.
Scene-linear metering, bloom, reconstruction and lighting remain unchanged.

## Ownership and lifetime

The selected backend owns native format negotiation, presentation pipelines,
and completed-use retirement. Format changes occur before drawable acquisition
and graph preparation; headroom-only changes update a frame-owned 16-byte
parameter record without allocating images. The graph's existing target-format
images follow the selected format. Failed transitions propagate an error.

At 1280×720, three Metal drawables plus one retained editor output gain
14.063 MiB when changing 4-byte pixels to RGBA16F. Vulkan additionally retains
one internal mirror per WSI image, so three WSI images, three mirrors and one
editor output gain 24.609 MiB. The supported eight-image Vulkan case gains
59.766 MiB. These approved figures are payload increases before allocator
alignment, not total renderer memory. No new intermediate rendering pass or
scene-history image is required.

## Consequences

Highlights can use available display range while SDR output remains available.
Windows uses a conservative full-frame ceiling rather than small-area peak
luminance. Actual range can change with display settings or window placement.
The feature increases output storage and can require a cold pipeline/target
transition. It does not introduce Rec.2020/PQ output or HDR10 mastering metadata.

## Alternatives considered

A fixed authored headroom is portable but can exceed the display's current
range. Scaling the entire SDR image brightens midtones and UI. Merely selecting
FP16 preserves the existing SDR ceiling. HDR10 requires a separate mastering
and metadata contract.

## Revisit when

Measurements justify removing Vulkan's present mirror, or wider-gamut/PQ
presentation and authored mastering controls become requirements.

## Verification

[Shared output selection](../../renderer/src/vkr_display_output.c) has a focused
boundary oracle for native units, offscreen fallback, unavailable/nonfinite
input and FP16 representability. The compiled shared shader helper checks the
SDR bypass, headroom-1 bootstrap, white anchor, dark-value preservation, hue,
component bounds and one-time scaling. Native Metal checks cover H=1/H=4,
scale 1/2.5, SDR fallback, re-enable and odd-size resize through normal frame
preparation. Headroom-only changes retain pipelines and target generation.
The 36-swatch numeric oracle has maximum absolute FP16 error 0.003899; an
opaque UI-white pixel is exactly 2.5 with native output scale 2.5. The same
lifecycle passes Metal API validation. These are local Release observations on
an Apple M1 Pro, not timing or cross-backend parity claims. The native commands
are `env -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION .scratch/edr-native-lifecycle`,
`python3 .scratch/check-edr-native.py`, and the separate diagnostic
`env -u MTL_SHADER_VALIDATION MTL_DEBUG_LAYER=1 .scratch/edr-native-lifecycle`.

The editor composite also preserves already-mapped FP16 values without a
second scale or SDR clamp. Its H=4/scale=2.5 swatch check has maximum error
0.003899 (`python3 .scratch/check-edr-native-editor-composite.py`).

Pre-change SDR, current SDR and AUTO offscreen captures are byte-identical.
An actual OS-backed window selects RGBA16F/extended-linear output at current
headroom 1 on the built-in display; synthetic metadata supplies the higher
headroom checks. This does not measure physical display brightness. Extended
final captures retain raw FP16 plus producer headroom/scale; comparison rejects
different or invalid producer display metadata under
[ADR-051](051-renderer-harness-and-evidence.md).

Generated Vulkan SPIR-V validates the 16-byte parameter layout and native root
strides; Metal startup reflection validates its consumed roots. Native
Vulkan execution is unavailable on this macOS host; affected shader entries
remain UNALIGNED under [ADR-044](044-shader-cross-backend-contract.md).
The local task evidence retains wrapper and
harness commands, report digests and limits. Original snapshot payloads remain
local; no baseline generation was promoted.
