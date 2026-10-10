#pragma once

#include "defines.h"

/* Bit-packed streams for schema messages (docs/proposals/network-protocol.md,
 * "Schema messages"). Values pack least significant bit first into a 64-bit
 * accumulator that flushes whole bytes, so a byte holds the low bits of the
 * first value it starts. A writer that runs out of room and a reader that
 * runs past its end set a flag instead of touching memory beyond the
 * buffer; callers check it once at the end. */

typedef struct VkrBitWriter {
  uint8_t *data;
  uint32_t capacity;
  uint32_t bytes;
  uint64_t accumulator;
  uint32_t pending;
  bool8_t overflow;
} VkrBitWriter;

typedef struct VkrBitReader {
  const uint8_t *data;
  uint32_t size;
  uint32_t bytes;
  uint64_t accumulator;
  uint32_t pending;
  bool8_t overflow;
} VkrBitReader;

void vkr_bit_writer_init(VkrBitWriter *writer, uint8_t *data,
                         uint32_t capacity);

/* Writes the low `bits` (0 to 64) bits of `value`. */
void vkr_bit_write(VkrBitWriter *writer, uint64_t value, uint32_t bits);

/* A 2-bit class selecting 8, 16, 32 or 64 bits, then the value. */
void vkr_bit_write_varuint(VkrBitWriter *writer, uint64_t value);

/* Zero bits up to the next byte boundary. */
void vkr_bit_write_align(VkrBitWriter *writer);

/* Aligns, then copies `size` bytes. */
void vkr_bit_write_bytes(VkrBitWriter *writer, const void *bytes,
                         uint32_t size);

/* Flushes the last partial byte and returns the size in bytes, or zero
   when the writer overflowed. */
uint32_t vkr_bit_writer_finish(VkrBitWriter *writer);

void vkr_bit_reader_init(VkrBitReader *reader, const uint8_t *data,
                         uint32_t size);

/* Reads `bits` (0 to 64) bits; zero once the reader overflowed. */
uint64_t vkr_bit_read(VkrBitReader *reader, uint32_t bits);

uint64_t vkr_bit_read_varuint(VkrBitReader *reader);

void vkr_bit_read_align(VkrBitReader *reader);

/* Aligns, then returns a view of `size` bytes, or NULL past the end. */
const uint8_t *vkr_bit_read_bytes(VkrBitReader *reader, uint32_t size);

/* True when every bit was consumed except zero padding in the last byte. */
bool8_t vkr_bit_reader_at_end(const VkrBitReader *reader);
