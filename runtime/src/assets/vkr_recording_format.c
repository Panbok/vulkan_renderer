#include "assets/vkr_recording_format.h"

#include "containers/str.h"
#include "core/vkr_byte_io.h"
#include "core/vkr_hash.h"

static const uint8_t VKR_RECORDING_FILE_MAGIC[8] = {'V', 'K', 'R', 'R',
                                                    'E', 'C', 'S', 'T'};
/* "VRCK" and "VRIX" read as little-endian u32. */
#define VKR_RECORDING_CHUNK_MAGIC 0x4B435256u
#define VKR_RECORDING_TRAILER_MAGIC 0x58495256u
/* Chunks smaller than this are stored raw; compression would not pay. */
#define VKR_RECORDING_COMPRESS_MIN 256u
#define VKR_RECORDING_ZSTD_LEVEL 3

/* ------------------------------------------------------------------------ */
/* Records                                                                   */

void vkr_recording_record_header_store(uint8_t *header, uint16_t stream,
                                       uint16_t flags, uint32_t size,
                                       uint64_t frame) {
  header[0] = (uint8_t)stream;
  header[1] = (uint8_t)(stream >> 8u);
  header[2] = (uint8_t)flags;
  header[3] = (uint8_t)(flags >> 8u);
  vkr_store_le_u32(header + 4u, size);
  vkr_store_le_u64(header + 8u, frame);
}

uint64_t vkr_recording_record_bytes(uint32_t payload_size) {
  return vkr_align_up_u64(
      (uint64_t)VKR_RECORDING_RECORD_HEADER_SIZE + payload_size, 8u);
}

bool8_t vkr_recording_records_next(const uint8_t *payload, uint64_t size,
                                   uint64_t *cursor,
                                   VkrRecordingRecord *out_record) {
  vkr_assert(payload != NULL || size == 0u);
  vkr_assert(cursor != NULL && out_record != NULL);
  if (*cursor >= size) {
    return false_v;
  }
  if (size - *cursor < VKR_RECORDING_RECORD_HEADER_SIZE) {
    *cursor = size;
    return false_v;
  }
  const uint8_t *header = payload + *cursor;
  const uint32_t record_size = vkr_load_le_u32(header + 4u);
  const uint64_t bytes = vkr_recording_record_bytes(record_size);
  if (bytes > size - *cursor) {
    *cursor = size;
    return false_v;
  }
  *out_record = (VkrRecordingRecord){
      .stream = (uint16_t)(header[0] | (header[1] << 8u)),
      .flags = (uint16_t)(header[2] | (header[3] << 8u)),
      .size = record_size,
      .frame = vkr_load_le_u64(header + 8u),
      .data = header + VKR_RECORDING_RECORD_HEADER_SIZE,
  };
  *cursor += bytes;
  return true_v;
}

/* ------------------------------------------------------------------------ */
/* Dictionary                                                                */

uint32_t vkr_recording_field_type_size(VkrRecordingFieldType type) {
  switch (type) {
  case VKR_RECORDING_FIELD_U8:
    return 1u;
  case VKR_RECORDING_FIELD_U16:
    return 2u;
  case VKR_RECORDING_FIELD_U32:
  case VKR_RECORDING_FIELD_I32:
  case VKR_RECORDING_FIELD_F32:
    return 4u;
  case VKR_RECORDING_FIELD_U64:
  case VKR_RECORDING_FIELD_I64:
  case VKR_RECORDING_FIELD_F64:
    return 8u;
  }
  return 0u;
}

const char *vkr_recording_field_type_name(VkrRecordingFieldType type) {
  switch (type) {
  case VKR_RECORDING_FIELD_U8:
    return "u8";
  case VKR_RECORDING_FIELD_U16:
    return "u16";
  case VKR_RECORDING_FIELD_U32:
    return "u32";
  case VKR_RECORDING_FIELD_U64:
    return "u64";
  case VKR_RECORDING_FIELD_I32:
    return "i32";
  case VKR_RECORDING_FIELD_I64:
    return "i64";
  case VKR_RECORDING_FIELD_F32:
    return "f32";
  case VKR_RECORDING_FIELD_F64:
    return "f64";
  }
  return "unknown";
}

vkr_internal bool8_t vkr_recording_name_valid(const char *name,
                                              bool8_t allow_empty) {
  if (!name) {
    return allow_empty;
  }
  const uint64_t length =
      string_length_bounded(name, VKR_RECORDING_NAME_MAX + 1u);
  return length <= VKR_RECORDING_NAME_MAX && (allow_empty || length > 0u);
}

vkr_internal bool8_t
vkr_recording_stream_valid(const VkrRecordingStream *stream) {
  if (!vkr_recording_name_valid(stream->name, false_v) ||
      stream->field_count > VKR_RECORDING_FIELD_MAX ||
      (stream->field_count > 0u && !stream->fields) ||
      (stream->record_size == 0u && stream->field_count > 0u)) {
    return false_v;
  }
  for (uint32_t i = 0u; i < stream->field_count; ++i) {
    const VkrRecordingField *field = &stream->fields[i];
    const uint64_t element = vkr_recording_field_type_size(field->type);
    const uint64_t end =
        (uint64_t)field->offset + element * (uint64_t)field->count;
    if (!vkr_recording_name_valid(field->name, false_v) ||
        !vkr_recording_name_valid(field->unit, true_v) || element == 0u ||
        field->count == 0u || end > stream->record_size) {
      return false_v;
    }
  }
  return true_v;
}

/* Stream: u16 id, u16 flags, u32 record size, u16 field count, name.
   Field: u8 type, u8 reserved, u16 count, u32 offset, name, unit.
   A name is u8 length then bytes. */
vkr_internal uint64_t vkr_recording_name_bytes(const char *name) {
  return 1u + (name ? string_length(name) : 0u);
}

vkr_internal uint8_t *vkr_recording_put_name(uint8_t *cursor,
                                             const char *name) {
  const uint64_t length = name ? string_length(name) : 0u;
  *cursor++ = (uint8_t)length;
  if (length > 0u) {
    MemCopy(cursor, name, length);
  }
  return cursor + length;
}

bool8_t vkr_recording_dictionary_encode(const VkrRecordingStream *streams,
                                        uint32_t stream_count,
                                        VkrAllocator *allocator,
                                        uint8_t **out_data,
                                        uint64_t *out_size) {
  vkr_assert(allocator != NULL && out_data != NULL && out_size != NULL);
  if (stream_count == 0u || stream_count > VKR_RECORDING_STREAM_MAX) {
    return false_v;
  }
  uint64_t size = 4u;
  for (uint32_t i = 0u; i < stream_count; ++i) {
    const VkrRecordingStream *stream = &streams[i];
    if (!vkr_recording_stream_valid(stream)) {
      return false_v;
    }
    size += 10u + vkr_recording_name_bytes(stream->name);
    for (uint32_t f = 0u; f < stream->field_count; ++f) {
      size += 8u + vkr_recording_name_bytes(stream->fields[f].name) +
              vkr_recording_name_bytes(stream->fields[f].unit);
    }
  }

  uint8_t *data =
      vkr_allocator_alloc(allocator, size, VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  if (!data) {
    return false_v;
  }
  uint8_t *cursor = data;
  vkr_store_le_u32(cursor, stream_count);
  cursor += 4u;
  for (uint32_t i = 0u; i < stream_count; ++i) {
    const VkrRecordingStream *stream = &streams[i];
    cursor[0] = (uint8_t)stream->id;
    cursor[1] = (uint8_t)(stream->id >> 8u);
    cursor[2] = (uint8_t)stream->flags;
    cursor[3] = (uint8_t)(stream->flags >> 8u);
    vkr_store_le_u32(cursor + 4u, stream->record_size);
    cursor[8] = (uint8_t)stream->field_count;
    cursor[9] = (uint8_t)(stream->field_count >> 8u);
    cursor = vkr_recording_put_name(cursor + 10u, stream->name);
    for (uint32_t f = 0u; f < stream->field_count; ++f) {
      const VkrRecordingField *field = &stream->fields[f];
      cursor[0] = (uint8_t)field->type;
      cursor[1] = 0u;
      cursor[2] = (uint8_t)field->count;
      cursor[3] = (uint8_t)(field->count >> 8u);
      vkr_store_le_u32(cursor + 4u, field->offset);
      cursor = vkr_recording_put_name(cursor + 8u, field->name);
      cursor = vkr_recording_put_name(cursor, field->unit);
    }
  }
  vkr_assert((uint64_t)(cursor - data) == size);
  *out_data = data;
  *out_size = size;
  return true_v;
}

typedef struct VkrRecordingByteReader {
  const uint8_t *data;
  uint64_t size;
  uint64_t cursor;
  bool8_t failed;
} VkrRecordingByteReader;

vkr_internal const uint8_t *vkr_recording_take(VkrRecordingByteReader *reader,
                                               uint64_t bytes) {
  if (reader->failed || bytes > reader->size - reader->cursor) {
    reader->failed = true_v;
    return NULL;
  }
  const uint8_t *at = reader->data + reader->cursor;
  reader->cursor += bytes;
  return at;
}

/* Copies a length-prefixed name into `strings`, NUL-terminated. */
vkr_internal const char *vkr_recording_take_name(VkrRecordingByteReader *reader,
                                                 char **strings) {
  const uint8_t *length = vkr_recording_take(reader, 1u);
  if (!length || *length > VKR_RECORDING_NAME_MAX) {
    reader->failed = true_v;
    return "";
  }
  const uint8_t *bytes = vkr_recording_take(reader, *length);
  if (!bytes) {
    return "";
  }
  char *name = *strings;
  MemCopy(name, bytes, *length);
  name[*length] = '\0';
  *strings += *length + 1u;
  return name;
}

/* One allocation holds the streams, then the fields, then the names. An
   encoded field takes at least 10 bytes and a name copy is no longer than its
   encoding, so the payload size bounds both parts. */
vkr_internal uint64_t vkr_recording_field_slots(uint64_t size) {
  return size / 10u + 1u;
}

bool8_t vkr_recording_dictionary_decode(const uint8_t *data, uint64_t size,
                                        VkrAllocator *allocator,
                                        VkrRecordingDictionary *out) {
  vkr_assert(allocator != NULL && out != NULL);
  MemZero(out, sizeof(*out));
  VkrRecordingByteReader reader = {.data = data, .size = size};
  const uint8_t *count_bytes = vkr_recording_take(&reader, 4u);
  if (!count_bytes) {
    return false_v;
  }
  const uint32_t stream_count = vkr_load_le_u32(count_bytes);
  if (stream_count == 0u || stream_count > VKR_RECORDING_STREAM_MAX) {
    return false_v;
  }

  const uint64_t stream_bytes =
      sizeof(VkrRecordingStream) * (uint64_t)stream_count;
  const uint64_t field_bytes =
      sizeof(VkrRecordingField) * vkr_recording_field_slots(size);
  const uint64_t block_bytes = stream_bytes + field_bytes + size;
  uint8_t *block = vkr_allocator_alloc(allocator, block_bytes,
                                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!block) {
    return false_v;
  }
  VkrRecordingStream *streams = (VkrRecordingStream *)block;
  VkrRecordingField *fields = (VkrRecordingField *)(block + stream_bytes);
  VkrRecordingField *fields_end =
      (VkrRecordingField *)(block + stream_bytes + field_bytes);
  char *strings = (char *)fields_end;

  for (uint32_t i = 0u; i < stream_count && !reader.failed; ++i) {
    const uint8_t *head = vkr_recording_take(&reader, 10u);
    if (!head) {
      break;
    }
    VkrRecordingStream *stream = &streams[i];
    stream->id = (uint16_t)(head[0] | (head[1] << 8u));
    stream->flags = (uint16_t)(head[2] | (head[3] << 8u));
    stream->record_size = vkr_load_le_u32(head + 4u);
    stream->field_count = (uint32_t)(head[8] | (head[9] << 8u));
    stream->name = vkr_recording_take_name(&reader, &strings);
    if (stream->field_count > VKR_RECORDING_FIELD_MAX ||
        fields + stream->field_count > fields_end) {
      reader.failed = true_v;
      break;
    }
    stream->fields = fields;
    for (uint32_t f = 0u; f < stream->field_count && !reader.failed; ++f) {
      const uint8_t *field_head = vkr_recording_take(&reader, 8u);
      if (!field_head) {
        break;
      }
      fields[f] = (VkrRecordingField){
          .type = (VkrRecordingFieldType)field_head[0],
          .count = (uint16_t)(field_head[2] | (field_head[3] << 8u)),
          .offset = vkr_load_le_u32(field_head + 4u),
      };
      fields[f].name = vkr_recording_take_name(&reader, &strings);
      fields[f].unit = vkr_recording_take_name(&reader, &strings);
    }
    fields += stream->field_count;
    if (!reader.failed && !vkr_recording_stream_valid(stream)) {
      reader.failed = true_v;
    }
  }

  if (reader.failed || reader.cursor != size) {
    vkr_allocator_free(allocator, block, block_bytes,
                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    return false_v;
  }
  out->streams = streams;
  out->stream_count = stream_count;
  out->block = block;
  out->block_bytes = block_bytes;
  return true_v;
}

void vkr_recording_dictionary_free(VkrAllocator *allocator,
                                   VkrRecordingDictionary *dictionary) {
  vkr_assert(allocator != NULL && dictionary != NULL);
  if (dictionary->block) {
    vkr_allocator_free(allocator, dictionary->block, dictionary->block_bytes,
                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  }
  MemZero(dictionary, sizeof(*dictionary));
}

const VkrRecordingStream *
vkr_recording_dictionary_find(const VkrRecordingDictionary *dictionary,
                              uint16_t id) {
  for (uint32_t i = 0u; i < dictionary->stream_count; ++i) {
    if (dictionary->streams[i].id == id) {
      return &dictionary->streams[i];
    }
  }
  return NULL;
}

/* ------------------------------------------------------------------------ */
/* End counters                                                              */

vkr_internal void vkr_recording_store_f64(uint8_t *dst, float64_t value) {
  uint64_t bits = 0u;
  MemCopy(&bits, &value, sizeof(bits));
  vkr_store_le_u64(dst, bits);
}

vkr_internal float64_t vkr_recording_load_f64(const uint8_t *src) {
  const uint64_t bits = vkr_load_le_u64(src);
  float64_t value = 0.0;
  MemCopy(&value, &bits, sizeof(value));
  return value;
}

void vkr_recording_end_encode(const VkrRecordingEnd *end,
                              uint8_t out[VKR_RECORDING_END_SIZE]) {
  vkr_store_le_u64(out + 0u, end->first_frame);
  vkr_store_le_u64(out + 8u, end->last_frame);
  vkr_store_le_u64(out + 16u, end->records);
  vkr_store_le_u64(out + 24u, end->dropped_records);
  vkr_store_le_u64(out + 32u, end->lost_records);
  vkr_store_le_u64(out + 40u, end->raw_bytes);
  vkr_store_le_u64(out + 48u, end->stored_bytes);
  vkr_store_le_u64(out + 56u, end->chunks);
  vkr_recording_store_f64(out + 64u, end->seconds);
  vkr_recording_store_f64(out + 72u, end->writer_seconds);
}

bool8_t vkr_recording_end_decode(const uint8_t *data, uint64_t size,
                                 VkrRecordingEnd *out) {
  if (size != VKR_RECORDING_END_SIZE) {
    return false_v;
  }
  *out = (VkrRecordingEnd){
      .first_frame = vkr_load_le_u64(data + 0u),
      .last_frame = vkr_load_le_u64(data + 8u),
      .records = vkr_load_le_u64(data + 16u),
      .dropped_records = vkr_load_le_u64(data + 24u),
      .lost_records = vkr_load_le_u64(data + 32u),
      .raw_bytes = vkr_load_le_u64(data + 40u),
      .stored_bytes = vkr_load_le_u64(data + 48u),
      .chunks = vkr_load_le_u64(data + 56u),
      .seconds = vkr_recording_load_f64(data + 64u),
      .writer_seconds = vkr_recording_load_f64(data + 72u),
  };
  return true_v;
}

const char *vkr_recording_chunk_type_name(uint32_t type) {
  switch (type) {
  case VKR_RECORDING_CHUNK_IDENTITY:
    return "identity";
  case VKR_RECORDING_CHUNK_DICTIONARY:
    return "dictionary";
  case VKR_RECORDING_CHUNK_RECORDS:
    return "records";
  case VKR_RECORDING_CHUNK_END:
    return "end";
  case VKR_RECORDING_CHUNK_INDEX:
    return "index";
  }
  return "unknown";
}

/* ------------------------------------------------------------------------ */
/* Writer                                                                    */

vkr_internal bool8_t vkr_recording_write(VkrRecordingWriter *writer,
                                         const void *data, uint64_t size) {
  if (writer->failed) {
    return false_v;
  }
  uint64_t written = 0u;
  if (size > 0u && (file_write(&writer->file, size, (const uint8_t *)data,
                               &written) != FILE_ERROR_NONE ||
                    written != size)) {
    writer->failed = true_v;
    return false_v;
  }
  writer->offset += size;
  return true_v;
}

bool8_t vkr_recording_writer_open(VkrRecordingWriter *writer,
                                  VkrAllocator *allocator, const char *path,
                                  const VkrCodec *codec) {
  vkr_assert(writer != NULL && allocator != NULL && path != NULL);
  MemZero(writer, sizeof(*writer));
  writer->allocator = allocator;
  writer->codec = codec;
  writer->level = VKR_RECORDING_ZSTD_LEVEL;
  writer->path = file_path_view(path);

  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  bitset8_set(&mode, FILE_MODE_CREATE);
  bitset8_set(&mode, FILE_MODE_TRUNCATE);
  bitset8_set(&mode, FILE_MODE_BINARY);
  if (file_open(&writer->path, mode, &writer->file) != FILE_ERROR_NONE) {
    return false_v;
  }
  writer->open = true_v;

  uint8_t header[VKR_RECORDING_FILE_HEADER_SIZE] = {0};
  MemCopy(header, VKR_RECORDING_FILE_MAGIC, sizeof(VKR_RECORDING_FILE_MAGIC));
  vkr_store_le_u32(header + 8u, VKR_RECORDING_FORMAT_VERSION);
  vkr_store_le_u32(header + 12u, VKR_RECORDING_FILE_HEADER_SIZE);
  return vkr_recording_write(writer, header, sizeof(header));
}

vkr_internal bool8_t vkr_recording_writer_reserve(VkrRecordingWriter *writer,
                                                  uint64_t capacity) {
  if (writer->scratch_capacity >= capacity) {
    return true_v;
  }
  if (writer->scratch) {
    vkr_allocator_free(writer->allocator, writer->scratch,
                       writer->scratch_capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  }
  writer->scratch = vkr_allocator_alloc(writer->allocator, capacity,
                                        VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  writer->scratch_capacity = writer->scratch ? capacity : 0u;
  return writer->scratch != NULL;
}

vkr_internal bool8_t vkr_recording_writer_note(VkrRecordingWriter *writer,
                                               VkrRecordingIndexEntry entry) {
  if (writer->index_count == writer->index_capacity) {
    const uint32_t capacity = Max(64u, writer->index_capacity * 2u);
    VkrRecordingIndexEntry *grown = vkr_allocator_realloc(
        writer->allocator, writer->index,
        sizeof(*grown) * (uint64_t)writer->index_capacity,
        sizeof(*grown) * (uint64_t)capacity, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!grown) {
      return false_v;
    }
    writer->index = grown;
    writer->index_capacity = capacity;
  }
  writer->index[writer->index_count++] = entry;
  return true_v;
}

bool8_t vkr_recording_writer_chunk(VkrRecordingWriter *writer,
                                   VkrRecordingChunkType type,
                                   uint64_t first_frame, uint64_t last_frame,
                                   const void *payload, uint64_t size) {
  vkr_assert(writer != NULL && (payload != NULL || size == 0u));
  if (!writer->open || writer->failed || size > VKR_RECORDING_CHUNK_MAX) {
    writer->failed = true_v;
    return false_v;
  }

  const uint8_t *stored = (const uint8_t *)payload;
  uint64_t stored_size = size;
  uint32_t codec = VKR_RECORDING_CODEC_RAW;
  /* The index stays raw: a reader checks it against the chunks in place. */
  if (writer->codec && writer->codec->bound && writer->codec->compress &&
      type != VKR_RECORDING_CHUNK_INDEX && size >= VKR_RECORDING_COMPRESS_MIN) {
    const uint64_t bound = writer->codec->bound(size);
    if (vkr_recording_writer_reserve(writer, bound)) {
      const uint64_t compressed = writer->codec->compress(
          writer->scratch, bound, payload, size, writer->level);
      if (compressed > 0u && compressed < size) {
        stored = writer->scratch;
        stored_size = compressed;
        codec = VKR_RECORDING_CODEC_ZSTD;
      }
    }
  }

  uint8_t header[VKR_RECORDING_CHUNK_HEADER_SIZE] = {0};
  vkr_store_le_u32(header + 0u, VKR_RECORDING_CHUNK_MAGIC);
  vkr_store_le_u32(header + 4u, (uint32_t)type);
  vkr_store_le_u64(header + 8u, first_frame);
  vkr_store_le_u64(header + 16u, last_frame);
  vkr_store_le_u32(header + 24u, (uint32_t)size);
  vkr_store_le_u32(header + 28u, (uint32_t)stored_size);
  vkr_store_le_u32(header + 32u, codec);
  vkr_store_le_u32(header + 36u, vkr_crc32(stored, stored_size));
  vkr_store_le_u32(header + 40u, vkr_crc32(header, 40u));

  const VkrRecordingIndexEntry entry = {
      .offset = writer->offset,
      .first_frame = first_frame,
      .last_frame = last_frame,
      .type = (uint32_t)type,
      .raw_size = (uint32_t)size,
  };
  if (!vkr_recording_write(writer, header, sizeof(header)) ||
      !vkr_recording_write(writer, stored, stored_size)) {
    return false_v;
  }
  writer->raw_bytes += size;
  writer->stored_bytes += sizeof(header) + stored_size;
  if (type != VKR_RECORDING_CHUNK_INDEX &&
      !vkr_recording_writer_note(writer, entry)) {
    writer->failed = true_v;
    return false_v;
  }
  return true_v;
}

vkr_internal void vkr_recording_writer_release(VkrRecordingWriter *writer) {
  if (writer->open) {
    file_close(&writer->file);
  }
  if (writer->scratch) {
    vkr_allocator_free(writer->allocator, writer->scratch,
                       writer->scratch_capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  }
  if (writer->index) {
    vkr_allocator_free(writer->allocator, writer->index,
                       sizeof(*writer->index) *
                           (uint64_t)writer->index_capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  writer->open = false_v;
  writer->scratch = NULL;
  writer->index = NULL;
}

vkr_internal bool8_t vkr_recording_writer_index(VkrRecordingWriter *writer) {
  const uint64_t size =
      8u + (uint64_t)writer->index_count * VKR_RECORDING_INDEX_ENTRY_SIZE;
  uint8_t *payload = vkr_allocator_alloc(writer->allocator, size,
                                         VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  if (!payload) {
    return false_v;
  }
  MemZero(payload, size);
  vkr_store_le_u32(payload, writer->index_count);
  for (uint32_t i = 0u; i < writer->index_count; ++i) {
    const VkrRecordingIndexEntry *entry = &writer->index[i];
    uint8_t *at = payload + 8u + (uint64_t)i * VKR_RECORDING_INDEX_ENTRY_SIZE;
    vkr_store_le_u64(at + 0u, entry->offset);
    vkr_store_le_u64(at + 8u, entry->first_frame);
    vkr_store_le_u64(at + 16u, entry->last_frame);
    vkr_store_le_u32(at + 24u, entry->type);
    vkr_store_le_u32(at + 28u, entry->raw_size);
  }
  const uint64_t index_offset = writer->offset;
  bool8_t ok = vkr_recording_writer_chunk(writer, VKR_RECORDING_CHUNK_INDEX, 0u,
                                          0u, payload, size);
  vkr_allocator_free(writer->allocator, payload, size,
                     VKR_ALLOCATOR_MEMORY_TAG_BUFFER);

  uint8_t trailer[VKR_RECORDING_TRAILER_SIZE] = {0};
  vkr_store_le_u32(trailer, VKR_RECORDING_TRAILER_MAGIC);
  vkr_store_le_u64(trailer + 8u, index_offset);
  return ok && vkr_recording_write(writer, trailer, sizeof(trailer));
}

bool8_t vkr_recording_writer_close(VkrRecordingWriter *writer) {
  vkr_assert(writer != NULL);
  if (!writer->open) {
    return false_v;
  }
  bool8_t ok = !writer->failed && vkr_recording_writer_index(writer);
  ok = ok && file_sync(&writer->file) == FILE_ERROR_NONE;
  vkr_recording_writer_release(writer);
  return ok;
}

void vkr_recording_writer_abandon(VkrRecordingWriter *writer) {
  vkr_assert(writer != NULL);
  vkr_recording_writer_release(writer);
}

/* ------------------------------------------------------------------------ */
/* Reader                                                                    */

/* Reads the chunk header at `offset`; false_v when it is not a whole, valid
   chunk inside the file. */
vkr_internal bool8_t vkr_recording_chunk_at(const VkrRecordingReader *reader,
                                            uint64_t offset,
                                            VkrRecordingChunkInfo *out) {
  if (offset > reader->size ||
      reader->size - offset < VKR_RECORDING_CHUNK_HEADER_SIZE) {
    return false_v;
  }
  const uint8_t *header = reader->data + offset;
  if (vkr_load_le_u32(header) != VKR_RECORDING_CHUNK_MAGIC ||
      vkr_load_le_u32(header + 40u) != vkr_crc32(header, 40u)) {
    return false_v;
  }
  *out = (VkrRecordingChunkInfo){
      .offset = offset,
      .type = vkr_load_le_u32(header + 4u),
      .first_frame = vkr_load_le_u64(header + 8u),
      .last_frame = vkr_load_le_u64(header + 16u),
      .raw_size = vkr_load_le_u32(header + 24u),
      .stored_size = vkr_load_le_u32(header + 28u),
      .codec = vkr_load_le_u32(header + 32u),
      .payload_crc = vkr_load_le_u32(header + 36u),
  };
  const uint64_t payload = offset + VKR_RECORDING_CHUNK_HEADER_SIZE;
  return out->raw_size <= VKR_RECORDING_CHUNK_MAX &&
         out->stored_size <= reader->size - payload &&
         (out->codec != VKR_RECORDING_CODEC_RAW ||
          out->stored_size == out->raw_size);
}

/* Walks chunk headers from the file header; stops at the first chunk that is
   cut off or damaged. Fills `chunks` when it is not NULL. */
vkr_internal uint32_t vkr_recording_scan(VkrRecordingReader *reader,
                                         VkrRecordingChunkInfo *chunks,
                                         uint64_t *out_end) {
  uint64_t offset = VKR_RECORDING_FILE_HEADER_SIZE;
  uint32_t count = 0u;
  VkrRecordingChunkInfo info;
  while (count < UINT32_MAX && vkr_recording_chunk_at(reader, offset, &info)) {
    const uint8_t *payload =
        reader->data + offset + VKR_RECORDING_CHUNK_HEADER_SIZE;
    if (vkr_crc32(payload, info.stored_size) != info.payload_crc) {
      break;
    }
    if (info.type != VKR_RECORDING_CHUNK_INDEX) {
      if (chunks) {
        chunks[count] = info;
      }
      ++count;
    }
    offset += VKR_RECORDING_CHUNK_HEADER_SIZE + info.stored_size;
  }
  *out_end = offset;
  return count;
}

/* Accepts the trailer's index only when it lists exactly the chunks a scan
   finds, so a reader never trusts an index over the data. */
vkr_internal bool8_t vkr_recording_index_matches(VkrRecordingReader *reader,
                                                 uint64_t scan_end) {
  if (reader->size - scan_end != VKR_RECORDING_TRAILER_SIZE) {
    return false_v;
  }
  const uint8_t *trailer = reader->data + scan_end;
  if (vkr_load_le_u32(trailer) != VKR_RECORDING_TRAILER_MAGIC) {
    return false_v;
  }
  VkrRecordingChunkInfo index;
  const uint64_t index_offset = vkr_load_le_u64(trailer + 8u);
  if (!vkr_recording_chunk_at(reader, index_offset, &index) ||
      index.type != VKR_RECORDING_CHUNK_INDEX ||
      index.codec != VKR_RECORDING_CODEC_RAW || index.raw_size < 8u) {
    return false_v;
  }
  const uint8_t *payload =
      reader->data + index_offset + VKR_RECORDING_CHUNK_HEADER_SIZE;
  const uint32_t count = vkr_load_le_u32(payload);
  if (count != reader->chunk_count ||
      8u + (uint64_t)count * VKR_RECORDING_INDEX_ENTRY_SIZE != index.raw_size) {
    return false_v;
  }
  for (uint32_t i = 0u; i < count; ++i) {
    const uint8_t *at =
        payload + 8u + (uint64_t)i * VKR_RECORDING_INDEX_ENTRY_SIZE;
    if (vkr_load_le_u64(at) != reader->chunks[i].offset) {
      return false_v;
    }
  }
  return true_v;
}

VkrRecordingReadError vkr_recording_reader_open(VkrRecordingReader *reader,
                                                VkrAllocator *allocator,
                                                const char *path) {
  vkr_assert(reader != NULL && allocator != NULL && path != NULL);
  MemZero(reader, sizeof(*reader));
  reader->allocator = allocator;
  const FilePath file = file_path_view(path);
  if (file_map_readonly(&file, &reader->mapping) != FILE_ERROR_NONE) {
    return VKR_RECORDING_READ_OPEN;
  }
  reader->data = reader->mapping.data;
  reader->size = reader->mapping.size;
  if (reader->size < VKR_RECORDING_FILE_HEADER_SIZE ||
      MemCompare(reader->data, VKR_RECORDING_FILE_MAGIC,
                 sizeof(VKR_RECORDING_FILE_MAGIC)) != 0) {
    vkr_recording_reader_close(reader);
    return VKR_RECORDING_READ_HEADER;
  }
  reader->version = vkr_load_le_u32(reader->data + 8u);
  if (reader->version != VKR_RECORDING_FORMAT_VERSION) {
    vkr_recording_reader_close(reader);
    return VKR_RECORDING_READ_VERSION;
  }

  uint64_t end = 0u;
  const uint32_t count = vkr_recording_scan(reader, NULL, &end);
  if (count > 0u) {
    reader->chunks = vkr_allocator_alloc(
        allocator, sizeof(*reader->chunks) * (uint64_t)count,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!reader->chunks) {
      vkr_recording_reader_close(reader);
      return VKR_RECORDING_READ_MEMORY;
    }
    vkr_recording_scan(reader, reader->chunks, &end);
  }
  reader->chunk_count = count;
  reader->valid_bytes = end;
  reader->indexed = vkr_recording_index_matches(reader, end);
  reader->truncated = vkr_recording_reader_find(
                          reader, VKR_RECORDING_CHUNK_END) == UINT32_MAX ||
                      (!reader->indexed && end != reader->size);
  return VKR_RECORDING_READ_OK;
}

void vkr_recording_reader_close(VkrRecordingReader *reader) {
  vkr_assert(reader != NULL);
  if (reader->chunks) {
    vkr_allocator_free(reader->allocator, reader->chunks,
                       sizeof(*reader->chunks) * (uint64_t)reader->chunk_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (reader->mapping.data) {
    file_unmap(&reader->mapping);
  }
  MemZero(reader, sizeof(*reader));
}

bool8_t vkr_recording_reader_chunk(const VkrRecordingReader *reader,
                                   uint32_t index, const VkrCodec *codec,
                                   VkrAllocator *allocator, uint8_t **out_data,
                                   uint64_t *out_size) {
  vkr_assert(reader != NULL && allocator != NULL);
  vkr_assert(out_data != NULL && out_size != NULL);
  *out_data = NULL;
  *out_size = 0u;
  if (index >= reader->chunk_count) {
    return false_v;
  }
  const VkrRecordingChunkInfo *info = &reader->chunks[index];
  const uint8_t *stored =
      reader->data + info->offset + VKR_RECORDING_CHUNK_HEADER_SIZE;
  if (vkr_crc32(stored, info->stored_size) != info->payload_crc) {
    return false_v;
  }
  if (info->codec != VKR_RECORDING_CODEC_RAW &&
      (info->codec != VKR_RECORDING_CODEC_ZSTD || !codec ||
       !codec->decompress)) {
    return false_v;
  }
  /* One spare byte keeps an empty payload a valid allocation. */
  uint8_t *data = vkr_allocator_alloc(allocator, (uint64_t)info->raw_size + 1u,
                                      VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  if (!data) {
    return false_v;
  }
  if (info->codec == VKR_RECORDING_CODEC_RAW) {
    MemCopy(data, stored, info->raw_size);
  } else if (codec->decompress(data, info->raw_size, stored,
                               info->stored_size) != info->raw_size) {
    vkr_allocator_free(allocator, data, (uint64_t)info->raw_size + 1u,
                       VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
    return false_v;
  }
  *out_data = data;
  *out_size = info->raw_size;
  return true_v;
}

void vkr_recording_reader_chunk_free(VkrAllocator *allocator, uint8_t *data,
                                     uint64_t size) {
  if (data) {
    vkr_allocator_free(allocator, data, size + 1u,
                       VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  }
}

uint32_t vkr_recording_reader_find(const VkrRecordingReader *reader,
                                   VkrRecordingChunkType type) {
  for (uint32_t i = 0u; i < reader->chunk_count; ++i) {
    if (reader->chunks[i].type == (uint32_t)type) {
      return i;
    }
  }
  return UINT32_MAX;
}
