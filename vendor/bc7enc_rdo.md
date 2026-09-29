# bc7enc_rdo encoders

`vendor/bc7enc_rdo/` holds unmodified files from
[richgel999/bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) at commit
`b9438627eef73a1157e84201b6fa6eb2ffd6d9f0`:

- `bc7e.ispc`: Binomial's BC7 encoder (Apache 2.0), compiled with the Intel
  Implicit SPMD Program Compiler (ISPC).
- `rgbcx.h`, `rgbcx.cpp`, `rgbcx_table4_small.h`: the scalar BC1-BC5 encoders
  (MIT or public domain); the texture packer uses only BC5.
- `LICENSE`: both licences.

The packer builds `bc` derived textures from them
([ADR-012](../docs/adr/012-texture-compression-pipeline.md)).
`cmake/vkr_bc7e.cmake` compiles `bc7e.ispc` for x86-64 with the ISPC found as
`VKR_ISPC_EXECUTABLE`, or else ISPC 1.31.0 downloaded at configure time into
the build directory and checked against its release SHA-256. Its SSE2 and
SSE4 targets are selected at run time; the eight-lane AVX2 target was slower
on Zen+ and its fused multiply-adds made output depend on the host
(ADR-012). `--opt=fast-math` is omitted: the
upstream source notes it makes no measurable difference, and without it the
output does not depend on the compiler's reassociation. rgbcx builds with
`RGBCX_USE_SMALLER_TABLES`, which only affects its BC1 tables.
Other architectures build without the encoder and reject the `bc` encoding.
