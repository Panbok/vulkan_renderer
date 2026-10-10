# DirectXTex BC6H and BC7 compute encoders

`vendor/directxtex/` holds the BC6H and BC7 compute shaders of
[microsoft/DirectXTex](https://github.com/microsoft/DirectXTex). The Vulkan
lightmap bake encodes desktop lamp-irradiance pages as BC6H and direction
pages as BC7 with them
([`tools/bake/vkr_bake_gpu.h`](../tools/bake/vkr_bake_gpu.h),
`vkr_bake_gpu_encode_bc6h` and `vkr_bake_gpu_encode_bc7`;
[desktop baked lamps](../docs/proposals/desktop-baked-lamps.md)).

| Upstream | Tag | Commit | Licence |
| --- | --- | --- | --- |
| [microsoft/DirectXTex](https://github.com/microsoft/DirectXTex) | `may2026` | `4feb3e11a020f35b796fc769a74216a555d4f5ef` | MIT, `LICENSE` |

## Files

- `Shaders/BC6HEncode.hlsl` (upstream `DirectXTex/Shaders/BC6HEncode.hlsl`)
  with the local edits below.
- `Shaders/BC7Encode.hlsl` (upstream `DirectXTex/Shaders/BC7Encode.hlsl`),
  unmodified.
- `LICENSE`, upstream's root licence.

DirectXTex's Direct3D 11 host code (`DirectXTex/BCDirectCompute.cpp`) is not
vendored. `tools/bake/vkr_bake_vulkan.cpp` reimplements its dispatch
sequence in Vulkan: the same kernels, constant buffer (`cbCS`, 32 bytes),
mode order and ping-pong between two error buffers. It differs in three
ways:

- A submission covers 32768 blocks instead of a dispatch per 64 blocks.
  Each submission is waited for, which keeps it far under the Windows TDR
  limit.
- The kernels are built with upstream's `REF_DEVICE`, so no group returns
  early and a group can encode up to 4 blocks past the last one. Direct3D
  drops those out-of-bounds accesses; Vulkan does not, so the host pads the
  error and block buffers to a multiple of 4 blocks and the source image
  with rows of zero texels.
- BC7 runs DirectXTex's default search: modes 4 to 6, then 1, 3 and 7. The
  three-subset modes 0 and 2 (`TEX_COMPRESS_BC7_USE_3SUBSETS`) are not run.

The rest of DirectXTex (the CPU codecs, image API, file formats, Direct3D
and WIC) is omitted. The CPU BC6H/BC7 codec that this directory held
before was removed: it took about 21 minutes (BC6H) and 25 minutes (BC7)
per 4096x4096 page with 4 threads on the Ryzen 5 2600.

## Local edits

Each edit is marked `// VKR:` in the source.

- `BC6HEncode.hlsl`, `EncodeBlockCS`: the call
  `finish_quantize(bBadQuantize, ...)` passed an `int` to an `out bool`
  parameter, which Slang rejects. A `bool` temporary takes the result and
  is copied into the `int`.
- `BC6HEncode.hlsl`, `quantize` and `unquantize`: these compare an `int2x3`
  with scalars, which Slang's SPIR-V matrix lowering stops on with an
  internal error (DXC's SPIR-V back end rejects them too). Their bodies are
  elementwise, so each now takes an `int3` row, and an `int2x3` overload
  runs it on both rows. In `unquantize` the sign mask `s` became `uint3`
  to match.

## Build

`tools/CMakeLists.txt` compiles each entry point (`TryModeG10CS`,
`TryModeLE10CS` and `EncodeBlockCS` of BC6H; `TryMode456CS`, `TryMode137CS`
and `EncodeBlockCS` of BC7) with `slangc -target spirv -profile spirv_1_5`
and shifts the D3D registers to Vulkan bindings: `b0` to 0, `t0` and `t1`
to 1 and 2, `u0` to 3. It disables two Slang warnings the upstream code
triggers: implicit integer-to-bool conversions (30081) and
non-short-circuiting `?:` (30056). `tools/bake/vkr_bake_spirv_embed.cmake`
embeds the modules in the bake tools.

The modules need Vulkan 1.3 `shaderIntegerDotProduct` (BC7's `dot` of
`uint` vectors), `shaderFloat16` and `shaderInt16` (BC6H's half
conversions), and the error report's decode needs `textureCompressionBC`.
A device without them bakes uncompressed desktop planes.

## Output

Encoded bytes are not compared across hosts or GPUs (owner decision
2026-10-09: each platform bakes, packages and tests on its own machine, and
only the Windows/Vulkan host writes BC planes). Metal has no encoder.
