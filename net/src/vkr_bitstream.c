#include "vkr_bitstream.h"

#include <string.h>

void vkr_bit_writer_init(VkrBitWriter *writer, uint8_t *data,
                         uint32_t capacity) {
  *writer = (VkrBitWriter){.data = data, .capacity = capacity};
}

static void bit_writer_flush_bytes(VkrBitWriter *writer) {
  while (writer->pending >= 8u) {
    if (writer->bytes >= writer->capacity) {
      writer->overflow = true_v;
      writer->pending = 0u;
      return;
    }
    writer->data[writer->bytes++] = (uint8_t)writer->accumulator;
    writer->accumulator >>= 8;
    writer->pending -= 8u;
  }
}

void vkr_bit_write(VkrBitWriter *writer, uint64_t value, uint32_t bits) {
  if (bits == 0u || writer->overflow) {
    return;
  }
  if (bits < 64u) {
    value &= (1ull << bits) - 1u;
  }
  /* At most 7 bits wait in the accumulator, so 32-bit halves always fit. */
  if (bits > 32u) {
    vkr_bit_write(writer, value & 0xffffffffull, 32u);
    vkr_bit_write(writer, value >> 32, bits - 32u);
    return;
  }
  writer->accumulator |= value << writer->pending;
  writer->pending += bits;
  bit_writer_flush_bytes(writer);
}

void vkr_bit_write_varuint(VkrBitWriter *writer, uint64_t value) {
  uint32_t size_class = 0u;
  if (value > 0xffu) {
    size_class = value > 0xffffu ? (value > 0xffffffffull ? 3u : 2u) : 1u;
  }
  vkr_bit_write(writer, size_class, 2u);
  vkr_bit_write(writer, value, 8u << size_class);
}

void vkr_bit_write_align(VkrBitWriter *writer) {
  if (writer->pending > 0u) {
    vkr_bit_write(writer, 0u, 8u - writer->pending);
  }
}

void vkr_bit_write_bytes(VkrBitWriter *writer, const void *bytes,
                         uint32_t size) {
  vkr_bit_write_align(writer);
  if (writer->overflow) {
    return;
  }
  if (size > writer->capacity - writer->bytes) {
    writer->overflow = true_v;
    return;
  }
  if (size > 0u) {
    memcpy(writer->data + writer->bytes, bytes, size);
  }
  writer->bytes += size;
}

uint32_t vkr_bit_writer_finish(VkrBitWriter *writer) {
  vkr_bit_write_align(writer);
  return writer->overflow ? 0u : writer->bytes;
}

void vkr_bit_reader_init(VkrBitReader *reader, const uint8_t *data,
                         uint32_t size) {
  *reader = (VkrBitReader){.data = data, .size = size};
}

uint64_t vkr_bit_read(VkrBitReader *reader, uint32_t bits) {
  if (bits == 0u || reader->overflow) {
    return 0u;
  }
  if (bits > 32u) {
    const uint64_t low = vkr_bit_read(reader, 32u);
    const uint64_t high = vkr_bit_read(reader, bits - 32u);
    return low | (high << 32);
  }
  while (reader->pending < bits) {
    if (reader->bytes >= reader->size) {
      reader->overflow = true_v;
      return 0u;
    }
    reader->accumulator |= (uint64_t)reader->data[reader->bytes++]
                           << reader->pending;
    reader->pending += 8u;
  }
  const uint64_t value = reader->accumulator & ((1ull << bits) - 1u);
  reader->accumulator >>= bits;
  reader->pending -= bits;
  return value;
}

uint64_t vkr_bit_read_varuint(VkrBitReader *reader) {
  const uint32_t size_class = (uint32_t)vkr_bit_read(reader, 2u);
  return vkr_bit_read(reader, 8u << size_class);
}

void vkr_bit_read_align(VkrBitReader *reader) {
  /* The bits left in the accumulator belong to the current byte. */
  const uint32_t skip = reader->pending % 8u;
  if (skip > 0u && vkr_bit_read(reader, skip) != 0u) {
    /* Padding must be zero: a canonical encoding has one form. */
    reader->overflow = true_v;
  }
}

const uint8_t *vkr_bit_read_bytes(VkrBitReader *reader, uint32_t size) {
  vkr_bit_read_align(reader);
  if (reader->overflow) {
    return NULL;
  }
  /* Whole bytes still in the accumulator come first; after alignment the
     accumulator holds none, because reads fill it a byte at a time and only
     as far as they need. */
  if (reader->pending != 0u || size > reader->size - reader->bytes) {
    reader->overflow = true_v;
    return NULL;
  }
  const uint8_t *view = reader->data + reader->bytes;
  reader->bytes += size;
  return view;
}

bool8_t vkr_bit_reader_at_end(const VkrBitReader *reader) {
  return !reader->overflow && reader->bytes == reader->size &&
         reader->pending < 8u && reader->accumulator == 0u;
}
