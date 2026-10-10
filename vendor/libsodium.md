# libsodium

`vendor/libsodium/` is a git submodule of
[jedisct1/libsodium](https://github.com/jedisct1/libsodium) at tag
`1.0.22-RELEASE` (commit `77e1ce5d6dee871c49ef211222ba18ef0c486bda`, ISC,
`LICENSE`), unmodified.

[cmake/vkr_libsodium.cmake](../cmake/vkr_libsodium.cmake) builds every C
source as the static target `vkr_sodium` with the configuration the
release's `build.zig` uses per host, and writes `builds/msvc/version.h` into
the build tree. The assembly files serve x86-64 Unix hosts only and are not
built. The network transport ([ADR-105](../docs/adr/105-network-transport-and-asset-depot.md))
uses X25519, ChaCha20-Poly1305, BLAKE2b and AES-256-GCM, whose hardware path
every CPU of ADR-083 has.
