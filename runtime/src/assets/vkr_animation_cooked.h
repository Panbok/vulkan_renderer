#pragma once

#include "assets/vkr_animation.h"

#define VKR_ANIMATION_COOKED_VERSION 1u
#define VKR_ANIMATION_COOKED_MAX_BYTES GB(1)

/* Version 1: explicit little-endian IEEE binary32 fields; no native structs.
 * Header (48 bytes): VKA1, u32 version, u64 size, u64 source fingerprint,
 * u64 FNV1a checksum (whole file with bytes 24..31 zero), u32 node/skin/clip
 * counts, u32 reserved=0. Payload is sequential, with u32 byte-length names:
 * node: name,parent,matrix-authored(u32),local(16 floats),TRS(3/4/3 floats);
 * node_order: node_count u32; skin: name,skeleton,joint_count,{joint,IBM[16]};
 * clip: name,duration,channel_count; channel:
 * node,path,interpolation,key_count, times[key_count],values[key_count * (cubic
 * ? 3 : 1)][4]. */

/* Distinct result arena and scoped scratch required. Success owns one result
 * allocation until its arena is reset. File and decoded storage each cap at
 * 1 GiB. No file bytes are borrowed. Failure
 * leaves output zero and allocates nothing in result. */
bool8_t vkr_animation_cooked_decode(VkrAllocator *result_allocator,
                                    VkrAllocator *scratch_allocator,
                                    const uint8_t *bytes, uint64_t size,
                                    VkrAnimationAsset *out_asset,
                                    const char **error);
