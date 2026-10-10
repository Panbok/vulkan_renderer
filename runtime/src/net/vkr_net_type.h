#pragma once

#include "core/vkr_type_desc.h"
#include "vkr_bitstream.h"

/* Descriptor-typed values on the wire (docs/proposals/network-protocol.md,
 * "Components on the wire"). A value travels as its authored properties in
 * descriptor order, without names:
 *
 *   BOOL                        1 bit
 *   I32                         zigzag bit varuint
 *   U32, ENUM                   bit varuint
 *   F32, ANGLE                  32 bits, exact
 *   VEC2 to VEC4, QUAT, COLOR,
 *   DIRECTION                   32 bits per component, exact
 *   STRING                      bit varuint length, then UTF-8 bytes
 *   ENTITY                      the 16-byte document-stable id
 *
 * TRANSIENT properties do not travel; a decoded value takes the type's
 * defaults for them. Both peers run the same build, so a type is the same
 * descriptor on both sides; the caller names it on the wire. Edits keep
 * exact floats: a value someone typed must arrive as typed. */

/* Writes `value`; false when a float is not finite or a string has no
   terminator within its capacity. */
bool8_t vkr_net_type_write(VkrBitWriter *writer, const VkrTypeDesc *type,
                           const void *value);

/* Reads a value of `type` into `out` (`type->size` bytes) and validates it
   with vkr_type_validate. False, with a message in `error`, on any violation
   or overrun. */
bool8_t vkr_net_type_read(VkrBitReader *reader, const VkrTypeDesc *type,
                          void *out, char *error, uint32_t capacity);
