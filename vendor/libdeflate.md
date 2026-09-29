# libdeflate

`vendor/libdeflate/` holds unmodified files from
[ebiggers/libdeflate](https://github.com/ebiggers/libdeflate) at commit
`92e6a0db9fa848d742f9eb286c92afc60f2c3dda` (MIT, `COPYING`): the public
header, `common_defs.h`, and the zlib decompression subset of `lib/` with its
x86 and ARM CPU feature detection. Compression, gzip and CRC-32 are omitted.

[`vkr_image_decode_rgba8`](../runtime/src/assets/vkr_image_decode.c) inflates
8-bit RGB and RGBA PNGs with it; every other image goes to stb_image, which
produces the same pixels for the images the fast path accepts.
