#include "assets/vkr_recording_analysis.h"

#include "containers/str.h"
#include "containers/vkr_sort.h"
#include "core/vkr_byte_io.h"
#include "math/vkr_math.h"

#define VKR_RECORDING_DATA_PATH_MAX 4096u
/* A spike candidate exceeds its window median by this factor and by at
   least this many milliseconds. */
#define VKR_RECORDING_SPIKE_RATIO 1.2
#define VKR_RECORDING_SPIKE_MIN_EXCESS_MS 4.0
/* JSON numbers keep this many significant digits: a microsecond for
   frame times, a millimetre for positions within 100 m. */
#define VKR_RECORDING_JSON_DIGITS 6u
/* "false" is one byte longer than "true"; a measured header may hold three
   truncation flags that turn false. */
#define VKR_RECORDING_JSON_FLAG_SLACK 3u
/* Column of vkr_recording_frames_write_json that reads the frame time, and
   one that names no series. */
#define VKR_RECORDING_COLUMN_FRAME_MS (UINT32_MAX - 1u)
#define VKR_RECORDING_COLUMN_UNKNOWN UINT32_MAX
/* Player columns: x, y, z, then speed. */
#define VKR_RECORDING_COLUMN_PLAYER (UINT32_MAX - 5u)
/* Pass columns: one of these bits with the pass id. */
#define VKR_RECORDING_COLUMN_PASS_GPU 0x40000000u
#define VKR_RECORDING_COLUMN_PASS_CPU 0x20000000u
#define VKR_RECORDING_COLUMN_PASS_MASK 0x0000ffffu

/* ------------------------------------------------------------------------ */
/* Load                                                                      */

typedef struct VkrRecordingDataLoad {
  VkrRecordingData *data;
  VkrRecordingReader reader;
  const VkrCodec *codec;
  VkrRecordingDictionary dictionary;
  bool8_t has_dictionary;

  const VkrRecordingStream *frame;
  const VkrRecordingStream *camera;
  const VkrRecordingStream *marker;
  const VkrRecordingStream *event;
  const VkrRecordingStream *metrics;
  const VkrRecordingStream *pass_name;
  const VkrRecordingStream *passes;
  const VkrRecordingStream *player;
  const VkrRecordingField *player_position;
  const VkrRecordingField *player_velocity;
  const VkrRecordingField *display_dt;
  const VkrRecordingField *position;
  const VkrRecordingField *yaw;
  const VkrRecordingField *valid_mask;
  /* The metrics field of each series, in dictionary order. */
  const VkrRecordingField *metric_fields[VKR_RECORDING_FIELD_MAX];
  uint32_t metric_count;

  /* First pass counts; the second fills up to them. */
  uint64_t frame_count;
  uint32_t marker_capacity;
  uint32_t event_capacity;
  uint32_t pass_capacity;
  uint64_t pass_rows;
  bool8_t too_large;
} VkrRecordingDataLoad;

vkr_internal float64_t vkr_recording_analysis_load_f64(const uint8_t *src) {
  const uint64_t bits = vkr_load_le_u64(src);
  float64_t value = 0.0;
  MemCopy(&value, &bits, sizeof(value));
  return value;
}

/* Element `element` of a scalar or vector field, converted to float64. */
vkr_internal float64_t vkr_recording_field_value(const VkrRecordingField *field,
                                                 const uint8_t *record,
                                                 uint32_t element) {
  const uint8_t *at =
      record + field->offset +
      (uint64_t)element * vkr_recording_field_type_size(field->type);
  switch (field->type) {
  case VKR_RECORDING_FIELD_U8:
    return (float64_t)at[0];
  case VKR_RECORDING_FIELD_U16:
    return (float64_t)((uint32_t)at[0] | ((uint32_t)at[1] << 8u));
  case VKR_RECORDING_FIELD_U32:
    return (float64_t)vkr_load_le_u32(at);
  case VKR_RECORDING_FIELD_U64:
    return (float64_t)vkr_load_le_u64(at);
  case VKR_RECORDING_FIELD_I32:
    return (float64_t)(int32_t)vkr_load_le_u32(at);
  case VKR_RECORDING_FIELD_I64:
    return (float64_t)(int64_t)vkr_load_le_u64(at);
  case VKR_RECORDING_FIELD_F32:
    return (float64_t)vkr_load_le_f32(at);
  case VKR_RECORDING_FIELD_F64:
    return vkr_recording_analysis_load_f64(at);
  }
  return 0.0;
}

vkr_internal const VkrRecordingStream *
vkr_recording_find_stream(const VkrRecordingDictionary *dictionary,
                          const char *name) {
  for (uint32_t i = 0u; i < dictionary->stream_count; ++i) {
    if (string_equals(dictionary->streams[i].name, name)) {
      return &dictionary->streams[i];
    }
  }
  return NULL;
}

/* A field of a fixed-size stream with at least `count` elements, or NULL. */
vkr_internal const VkrRecordingField *
vkr_recording_find_field(const VkrRecordingStream *stream, const char *name,
                         uint16_t count) {
  if (!stream) {
    return NULL;
  }
  for (uint32_t i = 0u; i < stream->field_count; ++i) {
    const VkrRecordingField *field = &stream->fields[i];
    if (string_equals(field->name, name) && field->count >= count) {
      return field;
    }
  }
  return NULL;
}

vkr_internal VkrRecordingDataError vkr_recording_data_open(
    VkrRecordingReader *reader, VkrAllocator *allocator, const char *path) {
  VkrRecordingReadError error =
      vkr_recording_reader_open(reader, allocator, path);
  if (error == VKR_RECORDING_READ_OPEN) {
    /* A bundle directory holds stream.vkrs. */
    char stream[VKR_RECORDING_DATA_PATH_MAX];
    const int32_t length =
        string_format(stream, sizeof(stream), "%s/stream.vkrs", path);
    if (length < 0 || (uint64_t)length >= sizeof(stream)) {
      return VKR_RECORDING_DATA_OPEN;
    }
    error = vkr_recording_reader_open(reader, allocator, stream);
  }
  switch (error) {
  case VKR_RECORDING_READ_OK:
    return VKR_RECORDING_DATA_OK;
  case VKR_RECORDING_READ_OPEN:
    return VKR_RECORDING_DATA_OPEN;
  case VKR_RECORDING_READ_HEADER:
  case VKR_RECORDING_READ_VERSION:
    return VKR_RECORDING_DATA_HEADER;
  case VKR_RECORDING_READ_MEMORY:
    return VKR_RECORDING_DATA_MEMORY;
  }
  return VKR_RECORDING_DATA_HEADER;
}

/* Decodes the dictionary and resolves the streams and fields by name. */
vkr_internal VkrRecordingDataError
vkr_recording_data_dictionary(VkrRecordingDataLoad *load) {
  VkrAllocator *allocator = load->data->allocator;
  const uint32_t index =
      vkr_recording_reader_find(&load->reader, VKR_RECORDING_CHUNK_DICTIONARY);
  uint8_t *payload = NULL;
  uint64_t size = 0u;
  if (index == UINT32_MAX ||
      !vkr_recording_reader_chunk(&load->reader, index, load->codec, allocator,
                                  &payload, &size)) {
    return VKR_RECORDING_DATA_DICTIONARY;
  }
  load->has_dictionary = vkr_recording_dictionary_decode(
      payload, size, allocator, &load->dictionary);
  vkr_recording_reader_chunk_free(allocator, payload, size);
  if (!load->has_dictionary) {
    return VKR_RECORDING_DATA_DICTIONARY;
  }

  const VkrRecordingDictionary *dictionary = &load->dictionary;
  load->frame = vkr_recording_find_stream(dictionary, "frame");
  load->camera = vkr_recording_find_stream(dictionary, "camera");
  load->marker = vkr_recording_find_stream(dictionary, "marker");
  load->event = vkr_recording_find_stream(dictionary, "event");
  load->metrics = vkr_recording_find_stream(dictionary, "metrics");
  load->pass_name = vkr_recording_find_stream(dictionary, "pass_name");
  load->passes = vkr_recording_find_stream(dictionary, "passes");
  load->player = vkr_recording_find_stream(dictionary, "player");
  load->player_position =
      vkr_recording_find_field(load->player, "position", 3u);
  load->player_velocity =
      vkr_recording_find_field(load->player, "velocity", 3u);
  load->display_dt = vkr_recording_find_field(load->frame, "display_dt", 1u);
  load->position = vkr_recording_find_field(load->camera, "position", 3u);
  load->yaw = vkr_recording_find_field(load->camera, "yaw", 1u);
  load->valid_mask = vkr_recording_find_field(load->metrics, "valid", 1u);

  for (uint32_t i = 0u; load->metrics && i < load->metrics->field_count; ++i) {
    const VkrRecordingField *field = &load->metrics->fields[i];
    if (field == load->valid_mask || field->count != 1u) {
      continue;
    }
    load->metric_fields[load->metric_count++] = field;
  }
  return VKR_RECORDING_DATA_OK;
}

vkr_internal bool8_t vkr_recording_is_stream(const VkrRecordingStream *stream,
                                             const VkrRecordingRecord *record) {
  return stream && record->stream == stream->id;
}

/* A fixed-size record whose size matches its dictionary entry. */
vkr_internal bool8_t vkr_recording_record_fits(
    const VkrRecordingStream *stream, const VkrRecordingRecord *record) {
  return vkr_recording_is_stream(stream, record) && stream->record_size > 0u &&
         record->size == stream->record_size;
}

/* The row count of a passes record whose size matches it, else zero. */
vkr_internal uint32_t
vkr_recording_pass_record_rows(const VkrRecordingRecord *record) {
  if (record->size < VKR_RECORDING_PASSES_HEADER_SIZE) {
    return 0u;
  }
  const uint32_t rows = vkr_load_le_u32(record->data);
  const uint64_t size = VKR_RECORDING_PASSES_HEADER_SIZE +
                        (uint64_t)VKR_RECORDING_PASS_ROW_SIZE * rows;
  return rows <= VKR_RECORDING_PASS_MAX && size == record->size ? rows : 0u;
}

/* The pass id of a pass_name record that fits, else UINT32_MAX. */
vkr_internal uint32_t
vkr_recording_pass_name_id(const VkrRecordingRecord *record) {
  if (record->size < 4u ||
      record->size - 4u > (uint32_t)VKR_RECORDING_PASS_NAME_MAX) {
    return UINT32_MAX;
  }
  const uint32_t id = vkr_load_le_u32(record->data);
  return id < VKR_RECORDING_PASS_MAX ? id : UINT32_MAX;
}

typedef void (*VkrRecordingDataVisit)(VkrRecordingDataLoad *load,
                                      const VkrRecordingRecord *record);

/* Visits every record of every readable RECORDS chunk.
   @return The chunks that failed their CRC or decode and were skipped. */
vkr_internal uint32_t vkr_recording_data_records(VkrRecordingDataLoad *load,
                                                 VkrRecordingDataVisit visit) {
  VkrAllocator *allocator = load->data->allocator;
  uint32_t bad_chunks = 0u;
  for (uint32_t i = 0u; i < load->reader.chunk_count; ++i) {
    if (load->reader.chunks[i].type != VKR_RECORDING_CHUNK_RECORDS) {
      continue;
    }
    uint8_t *payload = NULL;
    uint64_t size = 0u;
    if (!vkr_recording_reader_chunk(&load->reader, i, load->codec, allocator,
                                    &payload, &size)) {
      ++bad_chunks;
      continue;
    }
    uint64_t cursor = 0u;
    VkrRecordingRecord record;
    while (vkr_recording_records_next(payload, size, &cursor, &record)) {
      visit(load, &record);
    }
    if (cursor != size) {
      ++bad_chunks;
    }
    vkr_recording_reader_chunk_free(allocator, payload, size);
  }
  return bad_chunks;
}

vkr_internal void vkr_recording_data_count(VkrRecordingDataLoad *load,
                                           const VkrRecordingRecord *record) {
  if (vkr_recording_is_stream(load->marker, record)) {
    if (record->size >= 4u) {
      ++load->marker_capacity;
    }
    return;
  }
  if (vkr_recording_is_stream(load->event, record)) {
    if (record->size >= 2u &&
        load->event_capacity < VKR_RECORDING_DATA_EVENT_MAX) {
      ++load->event_capacity;
    }
    return;
  }
  if (vkr_recording_is_stream(load->pass_name, record)) {
    const uint32_t id = vkr_recording_pass_name_id(record);
    if (id != UINT32_MAX) {
      load->pass_capacity = Max(load->pass_capacity, id + 1u);
    }
    return;
  }
  if (vkr_recording_is_stream(load->passes, record)) {
    load->pass_rows += vkr_recording_pass_record_rows(record);
    return;
  }
  const bool8_t framed = vkr_recording_is_stream(load->frame, record) ||
                         vkr_recording_is_stream(load->camera, record) ||
                         vkr_recording_is_stream(load->metrics, record);
  if (!framed) {
    return;
  }
  if (record->frame >= VKR_RECORDING_DATA_FRAME_MAX) {
    load->too_large = true_v;
    return;
  }
  load->frame_count = Max(load->frame_count, record->frame + 1u);
}

vkr_internal void
vkr_recording_data_fill_metrics(VkrRecordingDataLoad *load,
                                const VkrRecordingRecord *record) {
  VkrRecordingData *data = load->data;
  uint64_t mask = UINT64_MAX;
  if (load->valid_mask) {
    const uint8_t *at = record->data + load->valid_mask->offset;
    const uint32_t bytes =
        vkr_recording_field_type_size(load->valid_mask->type);
    mask = bytes == 8u   ? vkr_load_le_u64(at)
           : bytes == 4u ? (uint64_t)vkr_load_le_u32(at)
                         : UINT64_MAX;
  }
  for (uint32_t i = 0u; i < load->metric_count; ++i) {
    if (((mask >> i) & 1u) == 0u) {
      continue;
    }
    const float64_t value =
        vkr_recording_field_value(load->metric_fields[i], record->data, 0u);
    if (!vkr_is_finite_f64(value)) {
      continue;
    }
    data->series[i].values[record->frame] = value;
    data->series[i].valid[record->frame] = 1u;
  }
}

vkr_internal void
vkr_recording_data_fill_passes(VkrRecordingDataLoad *load,
                               const VkrRecordingRecord *record) {
  VkrRecordingData *data = load->data;
  const uint64_t frame = record->frame;
  const uint32_t rows = vkr_recording_pass_record_rows(record);
  if (!data->pass_block || rows == 0u || frame >= data->frame_count ||
      data->pass_row_counts[frame] > 0u) {
    return;
  }
  if (data->pass_row_count + rows > load->pass_rows) {
    data->pass_rows_cut += rows;
    return;
  }
  const uint64_t first = data->pass_row_count;
  const uint8_t *at = record->data + VKR_RECORDING_PASSES_HEADER_SIZE;
  uint32_t kept = 0u;
  for (uint32_t i = 0u; i < rows; ++i, at += VKR_RECORDING_PASS_ROW_SIZE) {
    const uint16_t pass = (uint16_t)(at[0] | (at[1] << 8u));
    /* A row whose name record was lost names no pass. */
    if (pass >= data->pass_count || data->passes[pass].name[0] == '\0') {
      continue;
    }
    const uint16_t flags = (uint16_t)(at[2] | (at[3] << 8u));
    const float32_t cpu_ms = vkr_load_le_f32(at + 4u);
    const float32_t gpu_ms = vkr_load_le_f32(at + 8u);
    const bool8_t gpu_valid = (flags & VKR_RECORDING_PASS_GPU_VALID) != 0u &&
                              vkr_is_finite_f32(gpu_ms) && gpu_ms >= 0.0f;
    data->pass_rows[first + kept++] = (VkrRecordingPassRow){
        .pass = pass,
        .flags = gpu_valid ? VKR_RECORDING_PASS_GPU_VALID : 0u,
        .cpu_ms = vkr_is_finite_f32(cpu_ms) ? Max(cpu_ms, 0.0f) : 0.0f,
        .gpu_ms = gpu_valid ? gpu_ms : 0.0f,
    };
  }
  data->pass_row_count += kept;
  data->pass_first[frame] = first;
  data->pass_row_counts[frame] = (uint16_t)kept;
  data->pass_frame_flags[frame] = (uint8_t)(vkr_load_le_u32(record->data + 4u) &
                                            VKR_RECORDING_PASSES_GPU_TIMED);
}

/* The index of event name `name` (`length` bytes), added when new;
   UINT32_MAX when the table is full. */
vkr_internal uint32_t vkr_recording_data_event_name(VkrRecordingData *data,
                                                    const uint8_t *name,
                                                    uint32_t length) {
  length = Min(length, (uint32_t)VKR_RECORDING_NAME_MAX);
  for (uint32_t i = 0u; i < data->event_name_count; ++i) {
    const char *known = data->event_names[i].name;
    if (string_length(known) == length &&
        MemCompare(known, name, length) == 0) {
      return i;
    }
  }
  if (data->event_name_count == VKR_RECORDING_DATA_EVENT_NAME_MAX) {
    return UINT32_MAX;
  }
  VkrRecordingDataEventName *entry = &data->event_names[data->event_name_count];
  MemCopy(entry->name, name, length);
  entry->name[length] = '\0';
  return data->event_name_count++;
}

/* An event record, named in the data's name table. */
vkr_internal void
vkr_recording_data_fill_event(VkrRecordingDataLoad *load,
                              const VkrRecordingRecord *record) {
  VkrRecordingData *data = load->data;
  VkrRecordingEventView view;
  if (!vkr_recording_event_parse(record->data, record->size, &view)) {
    return;
  }
  const uint32_t name_index =
      data->event_count < load->event_capacity
          ? vkr_recording_data_event_name(data, view.name, view.name_length)
          : UINT32_MAX;
  if (name_index == UINT32_MAX) {
    ++data->events_dropped;
    return;
  }
  data->events[data->event_count++] = (VkrRecordingDataEvent){
      .frame = record->frame,
      .name = name_index,
      .count = view.count,
      .ms = view.ms,
      .engine = view.engine,
  };
}

vkr_internal void vkr_recording_data_fill(VkrRecordingDataLoad *load,
                                          const VkrRecordingRecord *record) {
  VkrRecordingData *data = load->data;
  if (vkr_recording_is_stream(load->pass_name, record)) {
    const uint32_t id = vkr_recording_pass_name_id(record);
    if (data->pass_block && id < data->pass_count) {
      const uint32_t length = record->size - 4u;
      MemCopy(data->passes[id].name, record->data + 4u, length);
      data->passes[id].name[length] = '\0';
    }
    return;
  }
  if (vkr_recording_is_stream(load->passes, record)) {
    vkr_recording_data_fill_passes(load, record);
    return;
  }
  if (vkr_recording_is_stream(load->event, record)) {
    vkr_recording_data_fill_event(load, record);
    return;
  }
  if (vkr_recording_is_stream(load->marker, record)) {
    if (record->size < 4u || data->marker_count >= load->marker_capacity) {
      return;
    }
    VkrRecordingDataMarker *marker = &data->markers[data->marker_count++];
    marker->frame = record->frame;
    marker->kind = vkr_load_le_u32(record->data);
    const uint32_t length =
        Min(record->size - 4u, (uint32_t)VKR_RECORDING_DATA_LABEL_MAX);
    MemCopy(marker->label, record->data + 4u, length);
    marker->label[length] = '\0';
    return;
  }
  if (record->frame >= data->frame_count) {
    return;
  }
  const uint64_t frame = record->frame;
  if (vkr_recording_record_fits(load->frame, record) && load->display_dt) {
    const float64_t display_dt =
        vkr_recording_field_value(load->display_dt, record->data, 0u);
    if (vkr_is_finite_f64(display_dt)) {
      data->display_dt[frame] = display_dt;
      data->frame_valid[frame] = 1u;
    }
  } else if (vkr_recording_record_fits(load->camera, record) &&
             load->position) {
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
      data->positions[frame * 3u + axis] =
          vkr_recording_field_value(load->position, record->data, axis);
    }
    data->yaws[frame] =
        load->yaw
            ? (float32_t)vkr_recording_field_value(load->yaw, record->data, 0u)
            : 0.0f;
    data->camera_valid[frame] = 1u;
  } else if (vkr_recording_record_fits(load->metrics, record)) {
    vkr_recording_data_fill_metrics(load, record);
  } else if (vkr_recording_record_fits(load->player, record) &&
             load->player_position) {
    /* Ticks arrive in order, so the frame keeps its last one. */
    float64_t speed_squared = 0.0;
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
      data->player_positions[frame * 3u + axis] =
          vkr_recording_field_value(load->player_position, record->data, axis);
      const float64_t velocity =
          load->player_velocity ? vkr_recording_field_value(
                                      load->player_velocity, record->data, axis)
                                : 0.0;
      speed_squared += velocity * velocity;
    }
    data->player_speeds[frame] = (float32_t)vkr_sqrt_f64(speed_squared);
    data->player_valid[frame] = 1u;
    data->has_player = true_v;
  }
}

/* One allocation: series and markers, then the float64 arrays, the float32
   yaws and speeds, and the byte flags. */
vkr_internal bool8_t vkr_recording_data_allocate(VkrRecordingDataLoad *load) {
  VkrRecordingData *data = load->data;
  const uint64_t frames = load->frame_count;
  const uint64_t series = load->metric_count;
  const uint64_t series_bytes =
      vkr_align_up_u64(sizeof(VkrRecordingSeries) * series, 8u);
  const uint64_t marker_bytes = vkr_align_up_u64(
      sizeof(VkrRecordingDataMarker) * (uint64_t)load->marker_capacity, 8u);
  const uint64_t event_bytes = vkr_align_up_u64(
      sizeof(VkrRecordingDataEvent) * (uint64_t)load->event_capacity +
          sizeof(VkrRecordingDataEventName) *
              (uint64_t)VKR_RECORDING_DATA_EVENT_NAME_MAX,
      8u);
  const uint64_t f64_bytes = sizeof(float64_t) * frames * (8u + series);
  const uint64_t f32_bytes =
      vkr_align_up_u64(sizeof(float32_t) * frames * 2u, 8u);
  const uint64_t flag_bytes = frames * (3u + series);
  const uint64_t block_bytes = series_bytes + marker_bytes + event_bytes +
                               f64_bytes + f32_bytes + flag_bytes;
  uint8_t *block = vkr_allocator_alloc(data->allocator, block_bytes,
                                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!block) {
    return false_v;
  }
  MemZero(block, block_bytes);
  data->block = block;
  data->block_bytes = block_bytes;
  data->frame_count = frames;
  data->series_count = (uint32_t)series;

  uint8_t *cursor = block;
  data->series = (VkrRecordingSeries *)cursor;
  cursor += series_bytes;
  data->markers = (VkrRecordingDataMarker *)cursor;
  cursor += marker_bytes;
  data->event_names = (VkrRecordingDataEventName *)cursor;
  data->events = (VkrRecordingDataEvent *)(data->event_names +
                                           VKR_RECORDING_DATA_EVENT_NAME_MAX);
  cursor += event_bytes;
  float64_t *f64 = (float64_t *)cursor;
  data->times = f64;
  data->display_dt = f64 + frames;
  data->positions = f64 + frames * 2u;
  data->player_positions = f64 + frames * 5u;
  for (uint64_t i = 0u; i < series; ++i) {
    data->series[i].values = f64 + frames * (8u + i);
  }
  cursor += f64_bytes;
  data->yaws = (float32_t *)cursor;
  data->player_speeds = data->yaws + frames;
  cursor += f32_bytes;
  data->frame_valid = cursor;
  data->camera_valid = cursor + frames;
  data->player_valid = cursor + frames * 2u;
  for (uint64_t i = 0u; i < series; ++i) {
    data->series[i].valid = cursor + frames * (3u + i);
    const VkrRecordingField *field = load->metric_fields[i];
    MemCopy(data->series[i].name, field->name, string_length(field->name));
    MemCopy(data->series[i].unit, field->unit, string_length(field->unit));
  }
  return true_v;
}

/* The pass block: names, then rows, then the per-frame row index, counts
   and flags. Nothing without pass names. */
vkr_internal bool8_t
vkr_recording_data_allocate_passes(VkrRecordingDataLoad *load) {
  VkrRecordingData *data = load->data;
  if (load->pass_capacity == 0u) {
    return true_v;
  }
  const uint64_t frames = data->frame_count;
  load->pass_rows = Min(load->pass_rows, VKR_RECORDING_DATA_PASS_ROW_MAX);
  const uint64_t name_bytes = vkr_align_up_u64(
      sizeof(VkrRecordingDataPass) * (uint64_t)load->pass_capacity, 8u);
  const uint64_t row_bytes =
      vkr_align_up_u64(sizeof(VkrRecordingPassRow) * load->pass_rows, 8u);
  const uint64_t first_bytes = sizeof(uint64_t) * frames;
  const uint64_t count_bytes = sizeof(uint16_t) * frames;
  const uint64_t block_bytes =
      name_bytes + row_bytes + first_bytes + count_bytes + frames;
  uint8_t *block = vkr_allocator_alloc(data->allocator, block_bytes,
                                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!block) {
    return false_v;
  }
  MemZero(block, block_bytes);
  data->pass_block = block;
  data->pass_block_bytes = block_bytes;
  data->passes = (VkrRecordingDataPass *)block;
  data->pass_count = load->pass_capacity;
  data->pass_rows = (VkrRecordingPassRow *)(block + name_bytes);
  data->pass_first = (uint64_t *)(block + name_bytes + row_bytes);
  data->pass_row_counts = (uint16_t *)(data->pass_first + frames);
  data->pass_frame_flags = (uint8_t *)(data->pass_row_counts + frames);
  return true_v;
}

/* By frame, then by name, then by count: a total order, so equal inputs
   sort alike. */
vkr_internal int32_t vkr_recording_compare_events(const void *lhs,
                                                  const void *rhs) {
  const VkrRecordingDataEvent *a = lhs;
  const VkrRecordingDataEvent *b = rhs;
  if (a->frame != b->frame) {
    return a->frame < b->frame ? -1 : 1;
  }
  if (a->name != b->name) {
    return a->name < b->name ? -1 : 1;
  }
  if (a->count != b->count) {
    return a->count < b->count ? -1 : 1;
  }
  return a->ms < b->ms ? -1 : (a->ms > b->ms ? 1 : 0);
}

/* Repeats the last frame and pose over lost ones, builds the clock and picks
   the frame-time series. */
vkr_internal void vkr_recording_data_finish(VkrRecordingData *data) {
  for (uint64_t i = 0u; i < data->frame_count; ++i) {
    if (!data->frame_valid[i]) {
      ++data->missing_frames;
      if (i > 0u) {
        data->display_dt[i] = data->display_dt[i - 1u];
      }
    }
    if (!data->camera_valid[i] && i > 0u) {
      MemCopy(&data->positions[i * 3u], &data->positions[(i - 1u) * 3u],
              sizeof(float64_t) * 3u);
      data->yaws[i] = data->yaws[i - 1u];
    }
    data->times[i] = i == 0u ? 0.0 : data->times[i - 1u] + data->display_dt[i];
  }
  for (uint32_t i = 0u; i < data->marker_count; ++i) {
    VkrRecordingDataMarker *marker = &data->markers[i];
    marker->time = data->times[Min(marker->frame, data->frame_count - 1u)];
  }
  /* The renderer's events arrive with their publication, a frame or two
     after events of later frames. */
  vkr_sort(data->events, data->event_count, sizeof(*data->events),
           vkr_recording_compare_events);
  for (uint64_t i = 0u; data->pass_block && i < data->frame_count; ++i) {
    if (data->pass_row_counts[i] == 0u) {
      continue;
    }
    ++data->pass_frames;
    data->pass_gpu_frames +=
        (data->pass_frame_flags[i] & VKR_RECORDING_PASSES_GPU_TIMED) ? 1u : 0u;
  }

  data->frame_time_series = UINT32_MAX;
  const uint32_t wall = vkr_recording_data_find_series(data, "frame.wall");
  if (wall == UINT32_MAX) {
    return;
  }
  for (uint64_t i = 0u; i < data->frame_count; ++i) {
    if (data->series[wall].valid[i]) {
      data->frame_time_series = wall;
      return;
    }
  }
}

VkrRecordingDataError vkr_recording_data_load(VkrRecordingData *out,
                                              VkrAllocator *allocator,
                                              const char *path,
                                              const VkrCodec *codec) {
  vkr_assert(out != NULL && allocator != NULL && path != NULL);
  MemZero(out, sizeof(*out));
  out->allocator = allocator;
  out->frame_time_series = UINT32_MAX;
  VkrRecordingDataLoad load = {.data = out, .codec = codec};
  VkrRecordingDataError error =
      vkr_recording_data_open(&load.reader, allocator, path);
  if (error != VKR_RECORDING_DATA_OK) {
    return error;
  }
  out->truncated = load.reader.truncated;

  error = vkr_recording_data_dictionary(&load);
  if (error != VKR_RECORDING_DATA_OK) {
    goto cleanup;
  }
  out->bad_chunks = vkr_recording_data_records(&load, vkr_recording_data_count);
  if (load.too_large) {
    error = VKR_RECORDING_DATA_TOO_LARGE;
    goto cleanup;
  }
  if (load.frame_count == 0u) {
    error = VKR_RECORDING_DATA_EMPTY;
    goto cleanup;
  }
  if (!vkr_recording_data_allocate(&load) ||
      !vkr_recording_data_allocate_passes(&load)) {
    error = VKR_RECORDING_DATA_MEMORY;
    goto cleanup;
  }
  (void)vkr_recording_data_records(&load, vkr_recording_data_fill);
  vkr_recording_data_finish(out);

cleanup:
  if (load.has_dictionary) {
    vkr_recording_dictionary_free(allocator, &load.dictionary);
  }
  vkr_recording_reader_close(&load.reader);
  if (error != VKR_RECORDING_DATA_OK) {
    vkr_recording_data_free(out);
  }
  return error;
}

void vkr_recording_data_free(VkrRecordingData *data) {
  vkr_assert(data != NULL);
  if (data->block) {
    vkr_allocator_free(data->allocator, data->block, data->block_bytes,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  if (data->pass_block) {
    vkr_allocator_free(data->allocator, data->pass_block,
                       data->pass_block_bytes, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  MemZero(data, sizeof(*data));
  data->frame_time_series = UINT32_MAX;
}

uint32_t vkr_recording_data_find_series(const VkrRecordingData *data,
                                        const char *name) {
  for (uint32_t i = 0u; i < data->series_count; ++i) {
    if (string_equals(data->series[i].name, name)) {
      return i;
    }
  }
  return UINT32_MAX;
}

bool8_t vkr_recording_data_frame_ms(const VkrRecordingData *data,
                                    uint64_t frame, float64_t *out_ms) {
  if (frame >= data->frame_count) {
    return false_v;
  }
  if (data->frame_time_series != UINT32_MAX) {
    const VkrRecordingSeries *series = &data->series[data->frame_time_series];
    if (!series->valid[frame]) {
      return false_v;
    }
    *out_ms = series->values[frame];
    return true_v;
  }
  if (!data->frame_valid[frame]) {
    return false_v;
  }
  *out_ms = data->display_dt[frame] * 1000.0;
  return true_v;
}

const char *vkr_recording_data_frame_source(const VkrRecordingData *data) {
  return data->frame_time_series != UINT32_MAX ? "frame.wall" : "display_dt";
}

const VkrRecordingPassRow *
vkr_recording_data_pass_rows(const VkrRecordingData *data, uint64_t frame,
                             uint32_t *out_count) {
  *out_count = 0u;
  if (!data->pass_block || frame >= data->frame_count ||
      data->pass_row_counts[frame] == 0u) {
    return NULL;
  }
  *out_count = data->pass_row_counts[frame];
  return &data->pass_rows[data->pass_first[frame]];
}

uint32_t vkr_recording_data_find_pass(const VkrRecordingData *data,
                                      const char *name) {
  for (uint32_t i = 0u; i < data->pass_count; ++i) {
    if (data->passes[i].name[0] != '\0' &&
        string_equals(data->passes[i].name, name)) {
      return i;
    }
  }
  return UINT32_MAX;
}

vkr_internal bool8_t vkr_recording_gpu_timed(const VkrRecordingData *data,
                                             uint64_t frame) {
  return data->pass_block && frame < data->frame_count &&
         data->pass_row_counts[frame] > 0u &&
         (data->pass_frame_flags[frame] & VKR_RECORDING_PASSES_GPU_TIMED);
}

uint64_t vkr_recording_data_gpu_frame_near(const VkrRecordingData *data,
                                           uint64_t frame, uint64_t reach) {
  if (!data->pass_block || frame >= data->frame_count) {
    return UINT64_MAX;
  }
  for (uint64_t distance = 0u; distance <= reach; ++distance) {
    if (distance <= frame && vkr_recording_gpu_timed(data, frame - distance)) {
      return frame - distance;
    }
    if (vkr_recording_gpu_timed(data, frame + distance)) {
      return frame + distance;
    }
    if (distance > frame && frame + distance >= data->frame_count) {
      break;
    }
  }
  return UINT64_MAX;
}

/* ------------------------------------------------------------------------ */
/* Analysis                                                                  */

typedef struct VkrRecordingSpikeRun {
  uint64_t first;
  uint64_t last;
  uint64_t worst;
  float64_t excess;
} VkrRecordingSpikeRun;

vkr_internal int32_t vkr_recording_compare_f64(const void *lhs,
                                               const void *rhs) {
  const float64_t a = *(const float64_t *)lhs;
  const float64_t b = *(const float64_t *)rhs;
  return a < b ? -1 : (a > b ? 1 : 0);
}

/* Largest excess first; the lower frame breaks ties. */
vkr_internal int32_t vkr_recording_compare_runs(const void *lhs,
                                                const void *rhs) {
  const VkrRecordingSpikeRun *a = lhs;
  const VkrRecordingSpikeRun *b = rhs;
  if (a->excess != b->excess) {
    return a->excess > b->excess ? -1 : 1;
  }
  return a->worst < b->worst ? -1 : (a->worst > b->worst ? 1 : 0);
}

/* The ceil(per_mille * count / 1000)-th smallest sample. */
vkr_internal float64_t vkr_recording_rank(const float64_t *sorted,
                                          uint64_t count, uint64_t per_mille) {
  uint64_t rank = (per_mille * count + 999u) / 1000u;
  rank = Clamp(rank, 1u, count);
  return sorted[rank - 1u];
}

/* Sorts `samples` and fills `out` from them. */
vkr_internal void vkr_recording_stats_compute(float64_t *samples,
                                              uint64_t count,
                                              VkrRecordingStats *out) {
  MemZero(out, sizeof(*out));
  out->valid = count;
  if (count == 0u) {
    return;
  }
  vkr_sort(samples, count, sizeof(float64_t), vkr_recording_compare_f64);
  float64_t sum = 0.0;
  for (uint64_t i = 0u; i < count; ++i) {
    sum += samples[i];
  }
  out->min = samples[0];
  out->max = samples[count - 1u];
  out->mean = sum / (float64_t)count;
  out->p50 = vkr_recording_rank(samples, count, 500u);
  out->p95 = vkr_recording_rank(samples, count, 950u);
  out->p99 = vkr_recording_rank(samples, count, 990u);
}

/* The nearest-rank median of the valid frame times in the window of
   VKR_RECORDING_ANALYSIS_WINDOW frames centred on `centre`, shifted inward
   at the ends of the recording. */
vkr_internal bool8_t vkr_recording_window_median(const float64_t *frame_ms,
                                                 const uint8_t *valid,
                                                 uint64_t count,
                                                 uint64_t centre,
                                                 float64_t *out_median) {
  const uint64_t window = VKR_RECORDING_ANALYSIS_WINDOW;
  uint64_t first = 0u;
  if (count > window) {
    first = centre > window / 2u ? centre - window / 2u : 0u;
    first = Min(first, count - window);
  }
  const uint64_t end = Min(first + window, count);
  float64_t samples[VKR_RECORDING_ANALYSIS_WINDOW];
  uint32_t sample_count = 0u;
  for (uint64_t i = first; i < end; ++i) {
    if (!valid[i]) {
      continue;
    }
    /* Insertion keeps the window sorted. */
    uint32_t at = sample_count;
    while (at > 0u && samples[at - 1u] > frame_ms[i]) {
      samples[at] = samples[at - 1u];
      --at;
    }
    samples[at] = frame_ms[i];
    ++sample_count;
  }
  if (sample_count == 0u) {
    return false_v;
  }
  *out_median = samples[(sample_count + 1u) / 2u - 1u];
  return true_v;
}

vkr_internal bool8_t vkr_recording_spike_candidate(float64_t frame_ms,
                                                   float64_t median,
                                                   float64_t budget_ms) {
  if (frame_ms > 2.0 * budget_ms) {
    return true_v;
  }
  return frame_ms > VKR_RECORDING_SPIKE_RATIO * median &&
         frame_ms - median >= VKR_RECORDING_SPIKE_MIN_EXCESS_MS;
}

/* Frame-time statistics, the time-based 1% low and budget misses. */
vkr_internal void vkr_recording_frame_stats(VkrRecordingAnalysis *analysis,
                                            const float64_t *frame_ms,
                                            const uint8_t *valid,
                                            uint64_t count,
                                            float64_t *samples) {
  uint64_t sample_count = 0u;
  for (uint64_t i = 0u; i < count; ++i) {
    if (!valid[i]) {
      continue;
    }
    samples[sample_count++] = frame_ms[i];
    analysis->over_budget += frame_ms[i] > analysis->budget_ms ? 1u : 0u;
    analysis->over_double_budget +=
        frame_ms[i] > 2.0 * analysis->budget_ms ? 1u : 0u;
  }
  vkr_recording_stats_compute(samples, sample_count, &analysis->frame_time);
  if (sample_count == 0u) {
    return;
  }

  const float64_t total = analysis->frame_time.mean * (float64_t)sample_count;
  const float64_t target = total * 0.01;
  float64_t covered = 0.0;
  analysis->low_1pct_ms = samples[0];
  for (uint64_t i = sample_count; i > 0u; --i) {
    covered += samples[i - 1u];
    if (covered >= target) {
      analysis->low_1pct_ms = samples[i - 1u];
      break;
    }
  }
}

/* The "ms" series, other than the frame time, that grew most at `frame`. */
vkr_internal void
vkr_recording_spike_contributors(const VkrRecordingData *data,
                                 const VkrRecordingAnalysis *analysis,
                                 VkrRecordingSpike *spike) {
  for (uint32_t i = 0u; i < data->series_count; ++i) {
    const VkrRecordingSeries *series = &data->series[i];
    /* cpu.frame_work is the frame time on the CPU, not one of its causes. */
    if (i == data->frame_time_series || !string_equals(series->unit, "ms") ||
        string_equals(series->name, "cpu.frame_work") ||
        !series->valid[spike->frame] || analysis->series[i].valid == 0u) {
      continue;
    }
    const float64_t value = series->values[spike->frame];
    const float64_t excess = value - analysis->series[i].p50;
    if (excess <= 0.0) {
      continue;
    }
    /* Insert in descending order; an equal excess stays behind the earlier
       series. */
    uint32_t at = spike->contributor_count;
    if (at == VKR_RECORDING_ANALYSIS_CONTRIBUTOR_MAX) {
      if (excess <= spike->contributors[at - 1u].excess) {
        continue;
      }
      --at;
    } else {
      ++spike->contributor_count;
    }
    while (at > 0u && spike->contributors[at - 1u].excess < excess) {
      spike->contributors[at] = spike->contributors[at - 1u];
      --at;
    }
    spike->contributors[at] = (VkrRecordingContributor){
        .series = i, .value = value, .excess = excess};
  }
}

/* For a spike the frame loop's wait for the renderer leads: the frame in
   flight whose GPU submission grew most (VkrRecordingSpike.gpu_frame). */
vkr_internal void
vkr_recording_spike_gpu_wait(const VkrRecordingData *data,
                             const VkrRecordingAnalysis *analysis,
                             VkrRecordingSpike *spike) {
  spike->gpu_frame = UINT64_MAX;
  if (spike->contributor_count == 0u ||
      !string_equals(data->series[spike->contributors[0].series].name,
                     "cpu.render_wait")) {
    return;
  }
  const uint32_t gpu = vkr_recording_data_find_series(data, "gpu.submission");
  if (gpu == UINT32_MAX || analysis->series[gpu].valid == 0u) {
    return;
  }
  const VkrRecordingSeries *series = &data->series[gpu];
  const float64_t floor = spike->contributors[0].excess * 0.25;
  const uint64_t first =
      spike->frame > VKR_RECORDING_ANALYSIS_GPU_LOOKBACK
          ? spike->frame - VKR_RECORDING_ANALYSIS_GPU_LOOKBACK
          : 0u;
  for (uint64_t f = first; f <= spike->frame; ++f) {
    if (!series->valid[f]) {
      continue;
    }
    const float64_t excess = series->values[f] - analysis->series[gpu].p50;
    if (excess >= floor &&
        (spike->gpu_frame == UINT64_MAX || excess > spike->gpu_excess)) {
      spike->gpu_frame = f;
      spike->gpu_ms = series->values[f];
      spike->gpu_excess = excess;
    }
  }
}

/* Largest GPU total first, then CPU total, then the lower id. A pass that
   runs in few frames ranks by what it cost over the recording, not by its
   cost when it ran. */
vkr_internal int32_t vkr_recording_compare_passes(const void *lhs,
                                                  const void *rhs) {
  const VkrRecordingPassStats *a = lhs;
  const VkrRecordingPassStats *b = rhs;
  const float64_t a_gpu = a->gpu.mean * (float64_t)a->gpu.valid;
  const float64_t b_gpu = b->gpu.mean * (float64_t)b->gpu.valid;
  if (a_gpu != b_gpu) {
    return a_gpu > b_gpu ? -1 : 1;
  }
  const float64_t a_cpu = a->cpu.mean * (float64_t)a->cpu.valid;
  const float64_t b_cpu = b->cpu.mean * (float64_t)b->cpu.valid;
  if (a_cpu != b_cpu) {
    return a_cpu > b_cpu ? -1 : 1;
  }
  return a->pass < b->pass ? -1 : (a->pass > b->pass ? 1 : 0);
}

/* Per-pass CPU and GPU statistics. Each pass's samples go to its own run of
   `scratch` (two per row), are sorted there and reduced. */
vkr_internal void vkr_recording_pass_stats(const VkrRecordingData *data,
                                           VkrRecordingAnalysis *out,
                                           uint64_t *offsets,
                                           float64_t *scratch) {
  const uint32_t passes = data->pass_count;
  const uint64_t rows = data->pass_row_count;
  float64_t *cpu = scratch;
  float64_t *gpu = scratch + rows;
  uint64_t *cpu_count = offsets;
  uint64_t *gpu_count = offsets + passes;
  uint64_t *start = offsets + 2u * passes;
  MemZero(offsets, sizeof(uint64_t) * 3u * passes);
  for (uint64_t i = 0u; i < rows; ++i) {
    ++start[data->pass_rows[i].pass];
  }
  uint64_t sum = 0u;
  for (uint32_t p = 0u; p < passes; ++p) {
    const uint64_t count = start[p];
    start[p] = sum;
    sum += count;
  }
  for (uint64_t i = 0u; i < rows; ++i) {
    const VkrRecordingPassRow *row = &data->pass_rows[i];
    const uint64_t at = start[row->pass];
    cpu[at + cpu_count[row->pass]++] = (float64_t)row->cpu_ms;
    if (row->flags & VKR_RECORDING_PASS_GPU_VALID) {
      gpu[at + gpu_count[row->pass]++] = (float64_t)row->gpu_ms;
    }
  }
  out->pass_count = 0u;
  for (uint32_t p = 0u; p < passes; ++p) {
    if (cpu_count[p] == 0u) {
      continue;
    }
    VkrRecordingPassStats *stats = &out->passes[out->pass_count++];
    stats->pass = p;
    vkr_recording_stats_compute(cpu + start[p], cpu_count[p], &stats->cpu);
    vkr_recording_stats_compute(gpu + start[p], gpu_count[p], &stats->gpu);
  }
  vkr_sort(out->passes, out->pass_count, sizeof(VkrRecordingPassStats),
           vkr_recording_compare_passes);
}

/* The frame whose pass rows explain a spike (VkrRecordingSpike.pass_frame). */
vkr_internal uint64_t vkr_recording_spike_pass_frame(
    const VkrRecordingData *data, const float64_t *frame_ms,
    const uint8_t *valid, const VkrRecordingSpike *spike, bool8_t *out_gpu) {
  *out_gpu = true_v;
  if (vkr_recording_gpu_timed(data, spike->frame)) {
    return spike->frame;
  }
  uint64_t best = UINT64_MAX;
  for (uint64_t f = spike->first_frame; f <= spike->last_frame; ++f) {
    if (valid[f] && vkr_recording_gpu_timed(data, f) &&
        (best == UINT64_MAX || frame_ms[f] > frame_ms[best])) {
      best = f;
    }
  }
  if (best != UINT64_MAX) {
    return best;
  }
  *out_gpu = false_v;
  uint32_t count = 0u;
  (void)vkr_recording_data_pass_rows(data, spike->frame, &count);
  return count > 0u ? spike->frame : UINT64_MAX;
}

/* The passes of `pass_frame` that grew most over their medians. `rank` maps
   a pass id to its index in analysis->passes. */
vkr_internal void
vkr_recording_spike_passes(const VkrRecordingData *data,
                           const VkrRecordingAnalysis *analysis,
                           const uint32_t *rank, const float64_t *frame_ms,
                           const uint8_t *valid, VkrRecordingSpike *spike) {
  spike->pass_frame = vkr_recording_spike_pass_frame(data, frame_ms, valid,
                                                     spike, &spike->pass_gpu);
  uint32_t count = 0u;
  const VkrRecordingPassRow *rows =
      vkr_recording_data_pass_rows(data, spike->pass_frame, &count);
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrRecordingPassRow *row = &rows[i];
    if (spike->pass_gpu && !(row->flags & VKR_RECORDING_PASS_GPU_VALID)) {
      continue;
    }
    const VkrRecordingPassStats *stats = &analysis->passes[rank[row->pass]];
    const float64_t value =
        spike->pass_gpu ? (float64_t)row->gpu_ms : (float64_t)row->cpu_ms;
    const float64_t excess =
        value - (spike->pass_gpu ? stats->gpu.p50 : stats->cpu.p50);
    if (excess <= 0.0) {
      continue;
    }
    uint32_t at = spike->pass_contributor_count;
    if (at == VKR_RECORDING_ANALYSIS_CONTRIBUTOR_MAX) {
      if (excess <= spike->passes[at - 1u].excess) {
        continue;
      }
      --at;
    } else {
      ++spike->pass_contributor_count;
    }
    while (at > 0u && spike->passes[at - 1u].excess < excess) {
      spike->passes[at] = spike->passes[at - 1u];
      --at;
    }
    spike->passes[at] = (VkrRecordingPassContributor){
        .pass = row->pass, .value = value, .excess = excess};
  }
}

bool8_t vkr_recording_analyze(const VkrRecordingData *data,
                              const VkrRecordingAnalysisOptions *options,
                              VkrAllocator *allocator,
                              VkrRecordingAnalysis *out) {
  vkr_assert(data != NULL && allocator != NULL && out != NULL);
  MemZero(out, sizeof(*out));
  out->allocator = allocator;
  out->budget_ms = options && options->budget_ms > 0.0
                       ? options->budget_ms
                       : VKR_RECORDING_ANALYSIS_BUDGET_MS;
  const uint32_t spike_max = options && options->spike_max > 0u
                                 ? options->spike_max
                                 : VKR_RECORDING_ANALYSIS_SPIKE_MAX;
  const uint64_t count = data->frame_count;

  /* Owned results: series statistics, the kept spikes, pass statistics. */
  const uint64_t stats_bytes = vkr_align_up_u64(
      sizeof(VkrRecordingStats) * (uint64_t)data->series_count, 8u);
  const uint64_t spike_bytes = sizeof(VkrRecordingSpike) * (uint64_t)spike_max;
  const uint64_t block_bytes =
      stats_bytes + spike_bytes +
      sizeof(VkrRecordingPassStats) * (uint64_t)data->pass_count;
  uint8_t *block = vkr_allocator_alloc(allocator, block_bytes,
                                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!block) {
    return false_v;
  }
  MemZero(block, block_bytes);
  out->block = block;
  out->block_bytes = block_bytes;
  out->series = (VkrRecordingStats *)block;
  out->spikes = (VkrRecordingSpike *)(block + stats_bytes);
  out->passes = (VkrRecordingPassStats *)(block + stats_bytes + spike_bytes);

  /* Scratch: frame times, sorted samples, window medians, spike runs, pass
     samples (two per row), pass counts and ranks, and the frame-time flags.
     Adjacent candidates merge, so runs are at most half the frames, rounded
     up. */
  const uint64_t run_capacity = (count + 1u) / 2u;
  const uint64_t pass_sample_count = 2u * data->pass_row_count;
  const uint64_t pass_offset_count = 3u * (uint64_t)data->pass_count;
  const uint64_t scratch_bytes =
      sizeof(float64_t) * (count * 3u + pass_sample_count) +
      sizeof(VkrRecordingSpikeRun) * run_capacity +
      sizeof(uint64_t) * pass_offset_count +
      vkr_align_up_u64(sizeof(uint32_t) * (uint64_t)data->pass_count, 8u) +
      count;
  uint8_t *scratch = vkr_allocator_alloc(allocator, scratch_bytes,
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!scratch) {
    vkr_recording_analysis_free(out);
    return false_v;
  }
  float64_t *frame_ms = (float64_t *)scratch;
  float64_t *samples = frame_ms + count;
  float64_t *medians = samples + count;
  float64_t *pass_samples = medians + count;
  VkrRecordingSpikeRun *runs =
      (VkrRecordingSpikeRun *)(pass_samples + pass_sample_count);
  uint64_t *pass_offsets = (uint64_t *)(runs + run_capacity);
  uint32_t *pass_rank = (uint32_t *)(pass_offsets + pass_offset_count);
  uint8_t *valid =
      (uint8_t *)pass_rank +
      vkr_align_up_u64(sizeof(uint32_t) * (uint64_t)data->pass_count, 8u);

  for (uint64_t i = 0u; i < count; ++i) {
    valid[i] = vkr_recording_data_frame_ms(data, i, &frame_ms[i]) ? 1u : 0u;
  }
  vkr_recording_frame_stats(out, frame_ms, valid, count, samples);
  for (uint32_t i = 0u; i < data->event_count; ++i) {
    const VkrRecordingDataEvent *event = &data->events[i];
    VkrRecordingEventTotal *total = &out->event_totals[event->name];
    ++total->records;
    total->count += event->count;
    total->ms += event->ms;
  }

  for (uint32_t s = 0u; s < data->series_count; ++s) {
    const VkrRecordingSeries *series = &data->series[s];
    uint64_t sample_count = 0u;
    for (uint64_t i = 0u; i < count; ++i) {
      if (series->valid[i]) {
        samples[sample_count++] = series->values[i];
      }
    }
    vkr_recording_stats_compute(samples, sample_count, &out->series[s]);
  }
  vkr_recording_pass_stats(data, out, pass_offsets, pass_samples);
  for (uint32_t i = 0u; i < data->pass_count; ++i) {
    pass_rank[i] = UINT32_MAX;
  }
  for (uint32_t i = 0u; i < out->pass_count; ++i) {
    pass_rank[out->passes[i].pass] = i;
  }

  /* Candidates, merged into runs that keep their worst frame. */
  uint64_t run_count = 0u;
  for (uint64_t i = 0u; i < count; ++i) {
    if (!valid[i] ||
        !vkr_recording_window_median(frame_ms, valid, count, i, &medians[i]) ||
        !vkr_recording_spike_candidate(frame_ms[i], medians[i],
                                       out->budget_ms)) {
      continue;
    }
    VkrRecordingSpikeRun *run = run_count > 0u ? &runs[run_count - 1u] : NULL;
    if (run && run->last + 1u == i) {
      run->last = i;
      if (frame_ms[i] > frame_ms[run->worst]) {
        run->worst = i;
      }
      continue;
    }
    runs[run_count++] =
        (VkrRecordingSpikeRun){.first = i, .last = i, .worst = i};
  }
  for (uint64_t r = 0u; r < run_count; ++r) {
    runs[r].excess = frame_ms[runs[r].worst] - medians[runs[r].worst];
  }
  vkr_sort(runs, run_count, sizeof(VkrRecordingSpikeRun),
           vkr_recording_compare_runs);

  out->spikes_found = run_count;
  out->spike_count = (uint32_t)Min(run_count, (uint64_t)spike_max);
  for (uint32_t k = 0u; k < out->spike_count; ++k) {
    const VkrRecordingSpikeRun *run = &runs[k];
    VkrRecordingSpike *spike = &out->spikes[k];
    const float64_t median = medians[run->worst];
    *spike = (VkrRecordingSpike){
        .frame = run->worst,
        .first_frame = run->first,
        .last_frame = run->last,
        .time = data->times[run->worst],
        .frame_ms = frame_ms[run->worst],
        .median_ms = median,
        .ratio = median > 0.0 ? frame_ms[run->worst] / median : 0.0,
        .excess_ms = run->excess,
        .pass_frame = UINT64_MAX,
    };
    vkr_recording_spike_contributors(data, out, spike);
    vkr_recording_spike_gpu_wait(data, out, spike);
    vkr_recording_spike_passes(data, out, pass_rank, frame_ms, valid, spike);
  }

  vkr_allocator_free(allocator, scratch, scratch_bytes,
                     VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return true_v;
}

void vkr_recording_analysis_free(VkrRecordingAnalysis *analysis) {
  vkr_assert(analysis != NULL);
  if (analysis->block) {
    vkr_allocator_free(analysis->allocator, analysis->block,
                       analysis->block_bytes, VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  }
  MemZero(analysis, sizeof(*analysis));
}

/* ------------------------------------------------------------------------ */
/* JSON                                                                      */

vkr_internal String8 vkr_recording_str(const char *text) {
  return string8_create_from_cstr((const uint8_t *)text, string_length(text));
}

vkr_internal void vkr_recording_json_key(VkrJsonWriter *json,
                                         const char *name) {
  vkr_json_writer_name(json, vkr_recording_str(name));
}

vkr_internal void vkr_recording_json_text(VkrJsonWriter *json,
                                          const char *text) {
  vkr_json_writer_string(json, vkr_recording_str(text));
}

/* Integers print as integers; other values keep a few significant digits.
   Non-finite values print as null. */
vkr_internal void vkr_recording_json_number(VkrJsonWriter *json,
                                            float64_t value) {
  if (!vkr_is_finite_f64(value)) {
    vkr_json_writer_null(json);
    return;
  }
  if (value == vkr_round_f64(value) && vkr_abs_f64(value) < 9.0e15) {
    vkr_json_writer_i64(json, (int64_t)value);
    return;
  }
  vkr_json_writer_f64_digits(json, value, VKR_RECORDING_JSON_DIGITS);
}

/* The number, or null when `present` is false. */
vkr_internal void vkr_recording_json_optional(VkrJsonWriter *json,
                                              bool8_t present,
                                              float64_t value) {
  if (present) {
    vkr_recording_json_number(json, value);
  } else {
    vkr_json_writer_null(json);
  }
}

vkr_internal void vkr_recording_json_named_optional(VkrJsonWriter *json,
                                                    const char *name,
                                                    bool8_t present,
                                                    float64_t value) {
  vkr_recording_json_key(json, name);
  vkr_recording_json_optional(json, present, value);
}

vkr_internal void vkr_recording_json_named_number(VkrJsonWriter *json,
                                                  const char *name,
                                                  float64_t value) {
  vkr_recording_json_key(json, name);
  vkr_recording_json_number(json, value);
}

vkr_internal void vkr_recording_json_named_u64(VkrJsonWriter *json,
                                               const char *name,
                                               uint64_t value) {
  vkr_recording_json_key(json, name);
  vkr_json_writer_u64(json, value);
}

vkr_internal bool8_t vkr_recording_is_bytes(const VkrRecordingSeries *series) {
  return string_equals(series->unit, "bytes");
}

vkr_internal void
vkr_recording_write_frame_time(VkrJsonWriter *json,
                               const VkrRecordingData *data,
                               const VkrRecordingAnalysis *analysis) {
  const VkrRecordingStats *stats = &analysis->frame_time;
  const bool8_t any = stats->valid > 0u;
  vkr_recording_json_key(json, "frame_time");
  vkr_json_writer_begin_object(json);
  vkr_recording_json_key(json, "source");
  vkr_recording_json_text(json, vkr_recording_data_frame_source(data));
  vkr_recording_json_key(json, "unit");
  vkr_recording_json_text(json, "ms");
  vkr_recording_json_named_u64(json, "valid", stats->valid);
  vkr_recording_json_named_optional(json, "min", any, stats->min);
  vkr_recording_json_named_optional(json, "max", any, stats->max);
  vkr_recording_json_named_optional(json, "mean", any, stats->mean);
  vkr_recording_json_named_optional(json, "p50", any, stats->p50);
  vkr_recording_json_named_optional(json, "p95", any, stats->p95);
  vkr_recording_json_named_optional(json, "p99", any, stats->p99);
  vkr_recording_json_named_optional(json, "low_1pct", any,
                                    analysis->low_1pct_ms);
  vkr_recording_json_named_number(json, "budget_ms", analysis->budget_ms);
  vkr_recording_json_named_u64(json, "over_budget", analysis->over_budget);
  vkr_recording_json_named_u64(json, "over_2x_budget",
                               analysis->over_double_budget);
  vkr_json_writer_end_object(json);
}

/* "name": [unit, valid, min, max, mean, p50, p95, p99] */
vkr_internal void vkr_recording_write_series_entry(
    VkrJsonWriter *json, const VkrRecordingData *data,
    const VkrRecordingAnalysis *analysis, uint32_t index) {
  const VkrRecordingSeries *series = &data->series[index];
  const VkrRecordingStats *stats = &analysis->series[index];
  const bool8_t any = stats->valid > 0u;
  vkr_recording_json_key(json, series->name);
  vkr_json_writer_begin_array(json);
  vkr_recording_json_text(json, series->unit);
  vkr_json_writer_u64(json, stats->valid);
  vkr_recording_json_optional(json, any, stats->min);
  vkr_recording_json_optional(json, any, stats->max);
  vkr_recording_json_optional(json, any, stats->mean);
  vkr_recording_json_optional(json, any, stats->p50);
  vkr_recording_json_optional(json, any, stats->p95);
  vkr_recording_json_optional(json, any, stats->p99);
  vkr_json_writer_end_array(json);
}

vkr_internal void vkr_recording_write_spike_markers(
    VkrJsonWriter *json, const VkrRecordingData *data, uint64_t frame) {
  const uint64_t reach = VKR_RECORDING_ANALYSIS_MARKER_FRAMES;
  uint32_t written = 0u;
  vkr_recording_json_key(json, "markers");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < data->marker_count &&
                        written < VKR_RECORDING_ANALYSIS_SPIKE_MARKER_MAX;
       ++i) {
    const VkrRecordingDataMarker *marker = &data->markers[i];
    if (marker->frame + reach < frame || marker->frame > frame + reach) {
      continue;
    }
    vkr_json_writer_begin_object(json);
    vkr_recording_json_named_u64(json, "frame", marker->frame);
    vkr_recording_json_key(json, "kind");
    vkr_recording_json_text(json, vkr_recording_marker_kind_name(marker->kind));
    vkr_recording_json_key(json, "label");
    vkr_recording_json_text(json, marker->label);
    vkr_json_writer_end_object(json);
    ++written;
  }
  vkr_json_writer_end_array(json);
}

/* An event as [name, frame, count, ms], ms null when not measured. */
vkr_internal void
vkr_recording_write_event(VkrJsonWriter *json, const VkrRecordingData *data,
                          const VkrRecordingDataEvent *event) {
  vkr_json_writer_begin_array(json);
  vkr_recording_json_text(json, data->event_names[event->name].name);
  vkr_json_writer_u64(json, event->frame);
  vkr_json_writer_u64(json, event->count);
  vkr_recording_json_optional(json, event->engine, (float64_t)event->ms);
  vkr_json_writer_end_array(json);
}

/* Engine events within the marker reach of `frame`, then script events,
   each in frame order, at most VKR_RECORDING_ANALYSIS_SPIKE_EVENT_MAX. */
vkr_internal void vkr_recording_write_spike_events(VkrJsonWriter *json,
                                                   const VkrRecordingData *data,
                                                   uint64_t frame) {
  const uint64_t reach = VKR_RECORDING_ANALYSIS_MARKER_FRAMES;
  uint32_t written = 0u;
  vkr_recording_json_key(json, "events");
  vkr_json_writer_begin_array(json);
  for (uint32_t pass = 0u; pass < 2u; ++pass) {
    for (uint32_t i = 0u; i < data->event_count &&
                          written < VKR_RECORDING_ANALYSIS_SPIKE_EVENT_MAX;
         ++i) {
      const VkrRecordingDataEvent *event = &data->events[i];
      if (event->engine != (pass == 0u) || event->frame + reach < frame ||
          event->frame > frame + reach) {
        continue;
      }
      vkr_recording_write_event(json, data, event);
      ++written;
    }
  }
  vkr_json_writer_end_array(json);
}

vkr_internal void
vkr_recording_write_spike(VkrJsonWriter *json, const VkrRecordingData *data,
                          const VkrRecordingAnalysis *analysis, uint32_t rank) {
  const VkrRecordingSpike *spike = &analysis->spikes[rank];
  const uint64_t frame = spike->frame;
  vkr_json_writer_begin_object(json);
  vkr_recording_json_named_u64(json, "rank", rank + 1u);
  vkr_recording_json_named_u64(json, "frame", frame);
  vkr_recording_json_named_u64(json, "frames",
                               spike->last_frame - spike->first_frame + 1u);
  vkr_recording_json_named_number(json, "time", spike->time);
  vkr_recording_json_named_number(json, "ms", spike->frame_ms);
  vkr_recording_json_named_number(json, "median", spike->median_ms);
  vkr_recording_json_named_number(json, "ratio", spike->ratio);

  vkr_recording_json_key(json, "position");
  if (data->camera_valid[frame]) {
    vkr_json_writer_begin_array(json);
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
      vkr_recording_json_number(json, data->positions[frame * 3u + axis]);
    }
    vkr_json_writer_end_array(json);
  } else {
    vkr_json_writer_null(json);
  }
  vkr_recording_json_named_optional(json, "yaw", data->camera_valid[frame],
                                    (float64_t)data->yaws[frame]);
  if (data->has_player) {
    vkr_recording_json_key(json, "player");
    if (data->player_valid[frame]) {
      vkr_json_writer_begin_array(json);
      for (uint32_t axis = 0u; axis < 3u; ++axis) {
        vkr_recording_json_number(json,
                                  data->player_positions[frame * 3u + axis]);
      }
      vkr_json_writer_end_array(json);
    } else {
      vkr_json_writer_null(json);
    }
  }

  vkr_recording_json_key(json, "contributors");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < spike->contributor_count; ++i) {
    const VkrRecordingContributor *contributor = &spike->contributors[i];
    vkr_json_writer_begin_object(json);
    vkr_recording_json_key(json, "name");
    vkr_recording_json_text(json, data->series[contributor->series].name);
    vkr_recording_json_named_number(json, "ms", contributor->value);
    vkr_recording_json_named_number(json, "excess", contributor->excess);
    vkr_json_writer_end_object(json);
  }
  vkr_json_writer_end_array(json);
  /* The GPU work the frame loop waited on, a few frames back. */
  vkr_recording_json_key(json, "gpu_wait");
  if (spike->gpu_frame != UINT64_MAX) {
    vkr_json_writer_begin_object(json);
    vkr_recording_json_named_number(json, "frame", (float64_t)spike->gpu_frame);
    vkr_recording_json_named_number(json, "ms", spike->gpu_ms);
    vkr_recording_json_named_number(json, "excess", spike->gpu_excess);
    vkr_json_writer_end_object(json);
  } else {
    vkr_json_writer_null(json);
  }

  if (data->pass_block) {
    vkr_recording_json_named_optional(json, "pass_frame",
                                      spike->pass_frame != UINT64_MAX,
                                      (float64_t)spike->pass_frame);
    vkr_recording_json_key(json, "pass_time");
    vkr_recording_json_text(json, spike->pass_gpu ? "gpu" : "cpu");
    vkr_recording_json_key(json, "passes");
    vkr_json_writer_begin_array(json);
    for (uint32_t i = 0u; i < spike->pass_contributor_count; ++i) {
      const VkrRecordingPassContributor *pass = &spike->passes[i];
      vkr_json_writer_begin_object(json);
      vkr_recording_json_key(json, "name");
      vkr_recording_json_text(json, data->passes[pass->pass].name);
      vkr_recording_json_named_number(json, "ms", pass->value);
      vkr_recording_json_named_number(json, "excess", pass->excess);
      vkr_json_writer_end_object(json);
    }
    vkr_json_writer_end_array(json);
  }

  vkr_recording_write_spike_markers(json, data, frame);
  vkr_recording_write_spike_events(json, data, frame);

  /* Aligned with the analysis "bytes_series" names. */
  vkr_recording_json_key(json, "bytes");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < data->series_count; ++i) {
    const VkrRecordingSeries *series = &data->series[i];
    if (!vkr_recording_is_bytes(series)) {
      continue;
    }
    vkr_recording_json_optional(json, series->valid[frame],
                                series->values[frame]);
  }
  vkr_json_writer_end_array(json);
  vkr_json_writer_end_object(json);
}

/* [name, frames, gpu_frames, gpu_mean, gpu_p50, gpu_p95, gpu_max,
   cpu_mean, cpu_p95] */
vkr_internal void vkr_recording_write_pass_entry(
    VkrJsonWriter *json, const VkrRecordingData *data,
    const VkrRecordingAnalysis *analysis, uint32_t index) {
  const VkrRecordingPassStats *stats = &analysis->passes[index];
  const bool8_t gpu = stats->gpu.valid > 0u;
  vkr_json_writer_begin_array(json);
  vkr_recording_json_text(json, data->passes[stats->pass].name);
  vkr_json_writer_u64(json, stats->cpu.valid);
  vkr_json_writer_u64(json, stats->gpu.valid);
  vkr_recording_json_optional(json, gpu, stats->gpu.mean);
  vkr_recording_json_optional(json, gpu, stats->gpu.p50);
  vkr_recording_json_optional(json, gpu, stats->gpu.p95);
  vkr_recording_json_optional(json, gpu, stats->gpu.max);
  vkr_recording_json_number(json, stats->cpu.mean);
  vkr_recording_json_number(json, stats->cpu.p95);
  vkr_json_writer_end_array(json);
}

vkr_internal void
vkr_recording_write_passes(VkrJsonWriter *json, const VkrRecordingData *data,
                           const VkrRecordingAnalysis *analysis,
                           uint32_t pass_limit) {
  static const char *const pass_columns[] = {
      "name",    "frames",  "gpu_frames", "gpu_mean", "gpu_p50",
      "gpu_p95", "gpu_max", "cpu_mean",   "cpu_p95"};
  vkr_recording_json_named_u64(json, "pass_frames", data->pass_frames);
  vkr_recording_json_named_u64(json, "pass_gpu_frames", data->pass_gpu_frames);
  vkr_recording_json_named_u64(json, "pass_rows_cut", data->pass_rows_cut);
  vkr_recording_json_key(json, "pass_columns");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < ArrayCount(pass_columns); ++i) {
    vkr_recording_json_text(json, pass_columns[i]);
  }
  vkr_json_writer_end_array(json);
  vkr_recording_json_key(json, "passes");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < pass_limit; ++i) {
    vkr_recording_write_pass_entry(json, data, analysis, i);
  }
  vkr_json_writer_end_array(json);
  vkr_recording_json_key(json, "passes_truncated");
  vkr_json_writer_bool(json, pass_limit < analysis->pass_count);
}

vkr_internal void
vkr_recording_write_analysis(VkrJsonWriter *json, const VkrRecordingData *data,
                             const VkrRecordingAnalysis *analysis,
                             bool8_t include_series, uint32_t series_limit,
                             uint32_t spike_limit, uint32_t pass_limit) {
  static const char *const series_columns[] = {"unit", "valid", "min", "max",
                                               "mean", "p50",   "p95", "p99"};
  vkr_json_writer_begin_object(json);
  vkr_recording_json_named_u64(json, "frames", data->frame_count);
  vkr_recording_json_named_u64(json, "missing_frames", data->missing_frames);
  vkr_recording_write_frame_time(json, data, analysis);

  if (include_series) {
    vkr_recording_json_key(json, "series_columns");
    vkr_json_writer_begin_array(json);
    for (uint32_t i = 0u; i < ArrayCount(series_columns); ++i) {
      vkr_recording_json_text(json, series_columns[i]);
    }
    vkr_json_writer_end_array(json);
    vkr_recording_json_key(json, "series");
    vkr_json_writer_begin_object(json);
    for (uint32_t i = 0u; i < series_limit; ++i) {
      vkr_recording_write_series_entry(json, data, analysis, i);
    }
    vkr_json_writer_end_object(json);
    vkr_recording_json_key(json, "series_truncated");
    vkr_json_writer_bool(json, series_limit < data->series_count);
  }
  if (data->pass_block) {
    vkr_recording_write_passes(json, data, analysis, pass_limit);
  }

  vkr_recording_json_key(json, "bytes_series");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < data->series_count; ++i) {
    if (vkr_recording_is_bytes(&data->series[i])) {
      vkr_recording_json_text(json, data->series[i].name);
    }
  }
  vkr_json_writer_end_array(json);

  /* [name, records, count, ms] per event name; ms null for script events. */
  vkr_recording_json_key(json, "events");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < data->event_name_count; ++i) {
    const VkrRecordingEventTotal *total = &analysis->event_totals[i];
    const bool8_t engine = string_n_equals(
        data->event_names[i].name, VKR_RECORDING_ENGINE_EVENT_PREFIX,
        string_length(VKR_RECORDING_ENGINE_EVENT_PREFIX));
    vkr_json_writer_begin_array(json);
    vkr_recording_json_text(json, data->event_names[i].name);
    vkr_json_writer_u64(json, total->records);
    vkr_json_writer_u64(json, total->count);
    vkr_recording_json_optional(json, engine, total->ms);
    vkr_json_writer_end_array(json);
  }
  vkr_json_writer_end_array(json);
  vkr_recording_json_named_u64(json, "events_dropped", data->events_dropped);

  vkr_recording_json_named_u64(json, "spikes_found", analysis->spikes_found);
  vkr_recording_json_key(json, "spikes");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < spike_limit; ++i) {
    vkr_recording_write_spike(json, data, analysis, i);
  }
  vkr_json_writer_end_array(json);
  vkr_recording_json_key(json, "spikes_truncated");
  vkr_json_writer_bool(json, spike_limit < analysis->spikes_found);
  vkr_json_writer_end_object(json);
}

vkr_internal bool8_t vkr_recording_count_sink(void *context,
                                              const uint8_t *data,
                                              uint64_t length) {
  (void)data;
  *(uint64_t *)context += length;
  return true_v;
}

typedef enum VkrRecordingMeasure {
  VKR_RECORDING_MEASURE_HEADER,
  VKR_RECORDING_MEASURE_SPIKE,
  VKR_RECORDING_MEASURE_SERIES,
  VKR_RECORDING_MEASURE_PASS,
} VkrRecordingMeasure;

/* Bytes one part of the analysis object takes when written. */
vkr_internal uint64_t vkr_recording_measure(
    const VkrRecordingData *data, const VkrRecordingAnalysis *analysis,
    bool8_t include_series, VkrRecordingMeasure part, uint32_t index) {
  uint64_t bytes = 0u;
  VkrJsonWriter counter;
  vkr_json_writer_init(&counter, vkr_recording_count_sink, &bytes);
  switch (part) {
  case VKR_RECORDING_MEASURE_HEADER:
    vkr_recording_write_analysis(&counter, data, analysis, include_series, 0u,
                                 0u, 0u);
    break;
  case VKR_RECORDING_MEASURE_SPIKE:
    vkr_recording_write_spike(&counter, data, analysis, index);
    break;
  case VKR_RECORDING_MEASURE_SERIES:
    /* A member needs an object around it; the braces are not counted. */
    vkr_json_writer_begin_object(&counter);
    vkr_recording_write_series_entry(&counter, data, analysis, index);
    vkr_json_writer_end_object(&counter);
    break;
  case VKR_RECORDING_MEASURE_PASS:
    /* An element needs an array around it; the brackets are not counted. */
    vkr_json_writer_begin_array(&counter);
    vkr_recording_write_pass_entry(&counter, data, analysis, index);
    vkr_json_writer_end_array(&counter);
    break;
  }
  vkr_json_writer_complete(&counter);
  const bool8_t wrapped = part == VKR_RECORDING_MEASURE_SERIES ||
                          part == VKR_RECORDING_MEASURE_PASS;
  return wrapped && bytes >= 2u ? bytes - 2u : bytes;
}

/* Takes parts `first` onward of one kind while they fit in `budget`, a
   comma before every part after a list's first; returns the new count. */
vkr_internal uint32_t vkr_recording_fit(const VkrRecordingData *data,
                                        const VkrRecordingAnalysis *analysis,
                                        bool8_t include_series,
                                        VkrRecordingMeasure part,
                                        uint32_t first, uint32_t last,
                                        uint64_t budget, uint64_t *used) {
  uint32_t count = first;
  for (uint32_t i = first; i < last; ++i) {
    const uint64_t bytes =
        vkr_recording_measure(data, analysis, include_series, part, i) +
        (i > 0u ? 1u : 0u);
    if (*used + bytes > budget) {
      break;
    }
    *used += bytes;
    ++count;
  }
  return count;
}

bool8_t vkr_recording_analysis_write_json(
    VkrJsonWriter *json, const VkrRecordingData *data,
    const VkrRecordingAnalysis *analysis,
    const VkrRecordingAnalysisJsonOptions *options) {
  vkr_assert(json != NULL && data != NULL && analysis != NULL);
  const VkrRecordingAnalysisJsonOptions defaults = {
      .budget_bytes = VKR_RECORDING_ANALYSIS_JSON_BUDGET,
      .include_series = true_v,
  };
  const VkrRecordingAnalysisJsonOptions *chosen = options ? options : &defaults;
  const bool8_t include_series = chosen->include_series;
  uint32_t series_limit = include_series ? data->series_count : 0u;
  uint32_t spike_limit = analysis->spike_count;
  uint32_t pass_limit = analysis->pass_count;

  if (chosen->budget_bytes > 0u) {
    /* Series come first, in dictionary order, then the first passes, then
       spikes in rank order, then the other passes; each part goes in while
       the object stays within the budget. */
    uint64_t used = vkr_recording_measure(data, analysis, include_series,
                                          VKR_RECORDING_MEASURE_HEADER, 0u) +
                    VKR_RECORDING_JSON_FLAG_SLACK;
    const uint32_t series_count = series_limit;
    series_limit = vkr_recording_fit(data, analysis, include_series,
                                     VKR_RECORDING_MEASURE_SERIES, 0u,
                                     series_count, chosen->budget_bytes, &used);
    pass_limit = vkr_recording_fit(
        data, analysis, include_series, VKR_RECORDING_MEASURE_PASS, 0u,
        Min(analysis->pass_count, VKR_RECORDING_ANALYSIS_JSON_PASSES_FIRST),
        chosen->budget_bytes, &used);
    spike_limit = vkr_recording_fit(
        data, analysis, include_series, VKR_RECORDING_MEASURE_SPIKE, 0u,
        analysis->spike_count, chosen->budget_bytes, &used);
    /* The rest of the passes continue the first ones' list. */
    if (pass_limit ==
        Min(analysis->pass_count, VKR_RECORDING_ANALYSIS_JSON_PASSES_FIRST)) {
      pass_limit = vkr_recording_fit(
          data, analysis, include_series, VKR_RECORDING_MEASURE_PASS,
          pass_limit, analysis->pass_count, chosen->budget_bytes, &used);
    }
  }

  vkr_recording_write_analysis(json, data, analysis, include_series,
                               series_limit, spike_limit, pass_limit);
  return !json->failed;
}

/* The series a frames column reads, VKR_RECORDING_COLUMN_FRAME_MS or
   VKR_RECORDING_COLUMN_UNKNOWN. */
vkr_internal uint32_t vkr_recording_column(const VkrRecordingData *data,
                                           const char *name) {
  if (string_equals(name, VKR_RECORDING_FRAME_MS_FIELD)) {
    return VKR_RECORDING_COLUMN_FRAME_MS;
  }
  static const char *const player_fields[] = {
      VKR_RECORDING_PLAYER_X_FIELD, VKR_RECORDING_PLAYER_Y_FIELD,
      VKR_RECORDING_PLAYER_Z_FIELD, VKR_RECORDING_PLAYER_SPEED_FIELD};
  for (uint32_t i = 0u; data->has_player && i < ArrayCount(player_fields);
       ++i) {
    if (string_equals(name, player_fields[i])) {
      return VKR_RECORDING_COLUMN_PLAYER + i;
    }
  }
  static const struct {
    const char *prefix;
    uint32_t bit;
  } pass_fields[] = {
      {VKR_RECORDING_PASS_GPU_FIELD, VKR_RECORDING_COLUMN_PASS_GPU},
      {VKR_RECORDING_PASS_CPU_FIELD, VKR_RECORDING_COLUMN_PASS_CPU},
  };
  for (uint32_t i = 0u; i < ArrayCount(pass_fields); ++i) {
    const uint64_t length = string_length(pass_fields[i].prefix);
    if (string_length(name) > length &&
        MemCompare(name, pass_fields[i].prefix, length) == 0) {
      const uint32_t pass = vkr_recording_data_find_pass(data, name + length);
      return pass == UINT32_MAX ? VKR_RECORDING_COLUMN_UNKNOWN
                                : pass_fields[i].bit | pass;
    }
  }
  return vkr_recording_data_find_series(data, name);
}

/* One pass's time in `frame`: false_v when the frame has no row for it or,
   for GPU, no GPU time. */
vkr_internal bool8_t vkr_recording_pass_cell(const VkrRecordingData *data,
                                             uint32_t column, uint64_t frame,
                                             float64_t *out_ms) {
  const uint32_t pass = column & VKR_RECORDING_COLUMN_PASS_MASK;
  const bool8_t gpu = (column & VKR_RECORDING_COLUMN_PASS_GPU) != 0u;
  uint32_t count = 0u;
  const VkrRecordingPassRow *rows =
      vkr_recording_data_pass_rows(data, frame, &count);
  for (uint32_t i = 0u; i < count; ++i) {
    if (rows[i].pass != pass) {
      continue;
    }
    if (gpu && !(rows[i].flags & VKR_RECORDING_PASS_GPU_VALID)) {
      return false_v;
    }
    *out_ms = gpu ? (float64_t)rows[i].gpu_ms : (float64_t)rows[i].cpu_ms;
    return true_v;
  }
  return false_v;
}

vkr_internal void vkr_recording_write_cell(VkrJsonWriter *json,
                                           const VkrRecordingData *data,
                                           uint32_t column, uint64_t frame) {
  if (column == VKR_RECORDING_COLUMN_FRAME_MS) {
    float64_t frame_ms = 0.0;
    const bool8_t present = vkr_recording_data_frame_ms(data, frame, &frame_ms);
    vkr_recording_json_optional(json, present, frame_ms);
    return;
  }
  if (column == VKR_RECORDING_COLUMN_UNKNOWN) {
    vkr_json_writer_null(json);
    return;
  }
  if (column >= VKR_RECORDING_COLUMN_PLAYER &&
      column < VKR_RECORDING_COLUMN_FRAME_MS) {
    const uint32_t axis = column - VKR_RECORDING_COLUMN_PLAYER;
    const bool8_t present = data->player_valid[frame] != 0u;
    vkr_recording_json_optional(json, present,
                                axis < 3u
                                    ? data->player_positions[frame * 3u + axis]
                                    : (float64_t)data->player_speeds[frame]);
    return;
  }
  if (column &
      (VKR_RECORDING_COLUMN_PASS_GPU | VKR_RECORDING_COLUMN_PASS_CPU)) {
    float64_t ms = 0.0;
    const bool8_t present = vkr_recording_pass_cell(data, column, frame, &ms);
    vkr_recording_json_optional(json, present, ms);
    return;
  }
  const VkrRecordingSeries *series = &data->series[column];
  vkr_recording_json_optional(json, series->valid[frame],
                              series->values[frame]);
}

bool8_t vkr_recording_data_has_field(const VkrRecordingData *data,
                                     const char *name) {
  return vkr_recording_column(data, name) != VKR_RECORDING_COLUMN_UNKNOWN;
}

/* Slowest GPU time first, then slowest CPU time, then the lower pass. */
vkr_internal int32_t vkr_recording_compare_rows(const void *lhs,
                                                const void *rhs) {
  const VkrRecordingPassRow *a = lhs;
  const VkrRecordingPassRow *b = rhs;
  if (a->gpu_ms != b->gpu_ms) {
    return a->gpu_ms > b->gpu_ms ? -1 : 1;
  }
  if (a->cpu_ms != b->cpu_ms) {
    return a->cpu_ms > b->cpu_ms ? -1 : 1;
  }
  return a->pass < b->pass ? -1 : (a->pass > b->pass ? 1 : 0);
}

bool8_t vkr_recording_passes_write_json(VkrJsonWriter *json,
                                        const VkrRecordingData *data,
                                        const VkrRecordingAnalysis *analysis,
                                        uint32_t top, uint64_t frame,
                                        uint64_t reach) {
  vkr_assert(json != NULL && data != NULL && analysis != NULL);
  const uint32_t limit =
      top > 0u ? Min(top, analysis->pass_count) : analysis->pass_count;
  vkr_json_writer_begin_object(json);
  vkr_recording_write_passes(json, data, analysis, limit);
  if (frame != UINT64_MAX) {
    uint64_t shown = frame;
    if (!vkr_recording_gpu_timed(data, frame)) {
      const uint64_t near =
          vkr_recording_data_gpu_frame_near(data, frame, reach);
      shown = near != UINT64_MAX ? near : frame;
    }
    uint32_t count = 0u;
    const VkrRecordingPassRow *rows =
        vkr_recording_data_pass_rows(data, shown, &count);
    VkrRecordingPassRow sorted[VKR_RECORDING_PASS_MAX];
    if (count > 0u) {
      MemCopy(sorted, rows, sizeof(*rows) * count);
      vkr_sort(sorted, count, sizeof(*sorted), vkr_recording_compare_rows);
    }
    vkr_recording_json_key(json, "frame");
    vkr_json_writer_begin_object(json);
    vkr_recording_json_named_u64(json, "requested", frame);
    vkr_recording_json_named_u64(json, "frame", shown);
    vkr_recording_json_key(json, "gpu");
    vkr_json_writer_bool(json, vkr_recording_gpu_timed(data, shown));
    vkr_recording_json_key(json, "rows");
    vkr_json_writer_begin_array(json);
    for (uint32_t i = 0u; i < count; ++i) {
      vkr_json_writer_begin_array(json);
      vkr_recording_json_text(json, data->passes[sorted[i].pass].name);
      vkr_recording_json_number(json, (float64_t)sorted[i].cpu_ms);
      vkr_recording_json_optional(
          json, (sorted[i].flags & VKR_RECORDING_PASS_GPU_VALID) != 0u,
          (float64_t)sorted[i].gpu_ms);
      vkr_json_writer_end_array(json);
    }
    vkr_json_writer_end_array(json);
    vkr_json_writer_end_object(json);
  }
  vkr_json_writer_end_object(json);
  return !json->failed;
}

bool8_t vkr_recording_frames_write_json(VkrJsonWriter *json,
                                        const VkrRecordingData *data,
                                        uint64_t first, uint64_t last,
                                        const char *const *fields,
                                        uint32_t field_count,
                                        uint32_t row_max) {
  vkr_assert(json != NULL && data != NULL);
  vkr_assert(fields != NULL || field_count == 0u);
  static const char *const fixed_columns[] = {"frame", "time", "x", "y", "z"};
  uint32_t columns[VKR_RECORDING_FIELD_MAX];
  const uint32_t column_count =
      Min(field_count, (uint32_t)VKR_RECORDING_FIELD_MAX);
  for (uint32_t i = 0u; i < column_count; ++i) {
    columns[i] = vkr_recording_column(data, fields[i]);
  }

  const uint64_t rows_cap =
      row_max > 0u ? row_max : VKR_RECORDING_FRAMES_ROW_MAX;
  uint64_t rows_total = 0u;
  if (data->frame_count > 0u && first <= last && first < data->frame_count) {
    rows_total = Min(last, data->frame_count - 1u) - first + 1u;
  }
  const uint64_t rows = Min(rows_total, rows_cap);

  vkr_json_writer_begin_object(json);
  vkr_recording_json_named_u64(json, "frames", data->frame_count);
  vkr_recording_json_named_u64(json, "first", first);
  vkr_recording_json_named_u64(json, "last", last);
  vkr_recording_json_key(json, "frame_source");
  vkr_recording_json_text(json, vkr_recording_data_frame_source(data));
  vkr_recording_json_key(json, "columns");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < ArrayCount(fixed_columns); ++i) {
    vkr_recording_json_text(json, fixed_columns[i]);
  }
  for (uint32_t i = 0u; i < column_count; ++i) {
    vkr_recording_json_text(json, fields[i]);
  }
  vkr_json_writer_end_array(json);

  vkr_recording_json_key(json, "rows");
  vkr_json_writer_begin_array(json);
  for (uint64_t row = 0u; row < rows; ++row) {
    const uint64_t frame = first + row;
    vkr_json_writer_begin_array(json);
    vkr_json_writer_u64(json, frame);
    vkr_recording_json_number(json, data->times[frame]);
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
      vkr_recording_json_optional(json, data->camera_valid[frame],
                                  data->positions[frame * 3u + axis]);
    }
    for (uint32_t i = 0u; i < column_count; ++i) {
      vkr_recording_write_cell(json, data, columns[i], frame);
    }
    vkr_json_writer_end_array(json);
  }
  vkr_json_writer_end_array(json);
  vkr_recording_json_named_u64(json, "rows_total", rows_total);
  vkr_recording_json_key(json, "truncated");
  vkr_json_writer_bool(json, rows < rows_total);

  /* The events of the written rows, as the analysis writes them. */
  const uint64_t end = first + rows;
  uint32_t events = 0u;
  uint64_t events_total = 0u;
  vkr_recording_json_key(json, "events");
  vkr_json_writer_begin_array(json);
  for (uint32_t i = 0u; i < data->event_count; ++i) {
    const VkrRecordingDataEvent *event = &data->events[i];
    if (event->frame < first || event->frame >= end) {
      continue;
    }
    ++events_total;
    if (events < VKR_RECORDING_FRAMES_EVENT_MAX) {
      vkr_recording_write_event(json, data, event);
      ++events;
    }
  }
  vkr_json_writer_end_array(json);
  vkr_recording_json_key(json, "events_truncated");
  vkr_json_writer_bool(json, events < events_total);
  vkr_json_writer_end_object(json);
  return !json->failed;
}
