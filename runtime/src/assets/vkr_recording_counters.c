#include "assets/vkr_recording_counters.h"

#include "assets/vkr_recording_format.h"
#include "containers/str.h"
#include "containers/vkr_sort.h"
#include "core/vkr_byte_io.h"
#include "filesystem/filesystem.h"
#include "math/vkr_math.h"
#include "memory/vkr_heap.h"
#include "platform/vkr_platform.h"

/* What `xctrace export` writes for a table: a schema naming the columns by
   mnemonic, then one <row> per event whose cells are the columns in order.
   A value appears in full once with an id, as
   <event-time id="1" fmt="00:00.073">73292458</event-time>, and later
   cells refer to it as <event-time ref="1"/>; ids count up through one
   export. A cell may nest elements, such as an os_signpost message's
   <uint64> inside its <os-log-metadata>. */

#define COUNTERS_XCRUN "/usr/bin/xcrun"
#define COUNTERS_XPATH                                                         \
  "/trace-toc/run[@number=\"1\"]/data/table[@schema=\"%s\"]"
/* The subsystem and event of vkr_platform_signpost_clock. */
#define COUNTERS_SUBSYSTEM "dev.vkr"
#define COUNTERS_CLOCK_EVENT "Clock"
#define COUNTERS_COLUMN_MAX 32u
#define COUNTERS_COLUMN_NAME_MAX 31u
/* Ids of the info table whose text is kept: its values are few. */
#define COUNTERS_TEXT_IDS 4096u
#define COUNTERS_TEXT_MAX 63u
#define COUNTERS_NUMBER_MAX 63u
#define COUNTERS_ELEMENT_DEPTH 8u
/* A row longer than this is malformed. */
#define COUNTERS_ROW_MAX (1u << 20u)
/* Counter ids the values table may name. */
#define COUNTERS_ID_SLOTS 256u
#define COUNTERS_PATH_MAX 4096u

/* What an id's value is, beside its number. */
#define COUNTERS_ID_CLOCK 1u
#define COUNTERS_ID_SUBSYSTEM 2u

static const char *const COUNTERS_SCHEMA[VKR_RECORDING_COUNTERS_TABLE_COUNT] = {
    "os-signpost",
    "gpu-counter-info",
    "gpu-counter-value",
};

/* One cell of a row: its own id or the id it refers to, its fmt attribute
   and its text (a nested element's for a cell without its own). */
typedef struct CountersCell {
  uint64_t id;
  bool8_t ref;
  const uint8_t *fmt;
  uint32_t fmt_length;
  const uint8_t *text;
  uint32_t text_length;
} CountersCell;

typedef struct CountersCounter {
  uint32_t id;
  char name[VKR_RECORDING_GPU_COUNTER_NAME_MAX + 1u];
  uint8_t unit;
  /* Per bucket from the trace's time zero: the samples' sum and count. */
  float64_t *sums;
  uint32_t *samples;
  uint64_t capacity;
} CountersCounter;

typedef struct CountersImport {
  const VkrRecordingCountersImport *config;
  VkrRecordingCountersResult *out;
  VkrRecordingCountersTable table;

  /* Bytes of the export not parsed yet. */
  uint8_t *buffer;
  uint64_t length;
  uint64_t capacity;
  bool8_t header_done;
  /* The current table's columns of interest, or UINT32_MAX. */
  uint32_t column_count;
  uint32_t col_time;
  uint32_t col_name;
  uint32_t col_subsystem;
  uint32_t col_message;
  uint32_t col_counter;
  uint32_t col_type;
  uint32_t col_value;

  /* Per id of the current export: its number (NaN for none), its
     COUNTERS_ID_* flags and, in the info table, its text. */
  float64_t *values;
  uint8_t *flags;
  uint64_t id_capacity;
  uint64_t id_used;
  char (*texts)[COUNTERS_TEXT_MAX + 1u];

  /* Each Clock event's CPU clock minus its trace time. */
  int64_t *offsets;
  uint32_t offset_count;
  uint32_t offset_capacity;

  CountersCounter counters[VKR_RECORDING_GPU_COUNTER_MAX];
  uint32_t counter_count;
  uint8_t slot_of[COUNTERS_ID_SLOTS];
  uint64_t bucket_ns;
  uint64_t last_bucket;
  bool8_t too_long;

  bool8_t failed;
  bool8_t stopped;
} CountersImport;

vkr_internal void counters_error(CountersImport *ci, const char *message) {
  if (!ci->out->error[0]) {
    string_format(ci->out->error, sizeof(ci->out->error), "%s", message);
  }
  ci->failed = true_v;
}

vkr_internal bool8_t counters_equals(const uint8_t *text, uint32_t length,
                                     const char *word) {
  return text && string_length(word) == length &&
         MemCompare(text, word, length) == 0;
}

/* The decimal number `text` holds, or NaN. */
vkr_internal float64_t counters_number(const uint8_t *text, uint32_t length) {
  char number[COUNTERS_NUMBER_MAX + 1u];
  float64_t value = 0.0;
  if (!text || length == 0u || length > COUNTERS_NUMBER_MAX) {
    return VKR_FLOAT_NAN;
  }
  MemCopy(number, text, length);
  number[length] = '\0';
  return string_to_f64(number, &value) ? value : VKR_FLOAT_NAN;
}

/* Makes room for id `id` in the per-id arrays. */
vkr_internal bool8_t counters_grow_ids(CountersImport *ci, uint64_t id) {
  if (id < ci->id_capacity) {
    return true_v;
  }
  const uint64_t grown = Max(Max(ci->id_capacity * 2u, id + 1u), 65536u);
  float64_t *values = vkr_heap_realloc(ci->values, sizeof(*values) * grown);
  if (!values) {
    return false_v;
  }
  ci->values = values;
  uint8_t *flags = vkr_heap_realloc(ci->flags, grown);
  if (!flags) {
    return false_v;
  }
  ci->flags = flags;
  for (uint64_t i = ci->id_capacity; i < grown; ++i) {
    ci->values[i] = VKR_FLOAT_NAN;
    ci->flags[i] = 0u;
  }
  ci->id_capacity = grown;
  return true_v;
}

/* Keeps what a value with an id says for the cells that refer to it. */
vkr_internal void counters_register(CountersImport *ci,
                                    const CountersCell *cell) {
  if (cell->ref || cell->id == UINT64_MAX || !counters_grow_ids(ci, cell->id)) {
    ci->failed = ci->failed || (!cell->ref && cell->id != UINT64_MAX);
    return;
  }
  ci->values[cell->id] = counters_number(cell->text, cell->text_length);
  uint8_t flags = 0u;
  if (counters_equals(cell->fmt, cell->fmt_length, COUNTERS_CLOCK_EVENT)) {
    flags |= COUNTERS_ID_CLOCK;
  }
  if (counters_equals(cell->fmt, cell->fmt_length, COUNTERS_SUBSYSTEM)) {
    flags |= COUNTERS_ID_SUBSYSTEM;
  }
  ci->flags[cell->id] = flags;
  ci->id_used = Max(ci->id_used, cell->id + 1u);
  if (ci->texts && cell->id < COUNTERS_TEXT_IDS) {
    const uint32_t length = Min(cell->text_length, COUNTERS_TEXT_MAX);
    if (length > 0u) {
      MemCopy(ci->texts[cell->id], cell->text, length);
    }
    ci->texts[cell->id][length] = '\0';
  }
}

/* Skips spaces and line breaks. */
vkr_internal const uint8_t *counters_skip(const uint8_t *at,
                                          const uint8_t *end) {
  while (at < end &&
         (*at == ' ' || *at == '\n' || *at == '\r' || *at == '\t')) {
    ++at;
  }
  return at;
}

/* Reads one element at `*at` into `cell`, its nested ones only for their
   ids and the first text, and moves `*at` past it. False when malformed. */
vkr_internal bool8_t counters_element(CountersImport *ci, const uint8_t **at,
                                      const uint8_t *end, CountersCell *cell,
                                      uint32_t depth) {
  const uint8_t *p = counters_skip(*at, end);
  *cell = (CountersCell){.id = UINT64_MAX};
  if (depth >= COUNTERS_ELEMENT_DEPTH || p >= end || *p != '<') {
    return false_v;
  }
  const uint8_t *tag = ++p;
  while (p < end && *p != ' ' && *p != '>' && *p != '/') {
    ++p;
  }
  const uint32_t tag_length = (uint32_t)(p - tag);
  /* Attributes: id, ref and fmt matter. */
  bool8_t empty = false_v;
  for (;;) {
    p = counters_skip(p, end);
    if (p >= end) {
      return false_v;
    }
    if (*p == '/') {
      if (p + 1 >= end || p[1] != '>') {
        return false_v;
      }
      p += 2;
      empty = true_v;
      break;
    }
    if (*p == '>') {
      ++p;
      break;
    }
    const uint8_t *name = p;
    while (p < end && *p != '=') {
      ++p;
    }
    const uint32_t name_length = (uint32_t)(p - name);
    if (p + 1 >= end || p[1] != '"') {
      return false_v;
    }
    p += 2;
    const uint8_t *value = p;
    while (p < end && *p != '"') {
      ++p;
    }
    if (p >= end) {
      return false_v;
    }
    const uint32_t value_length = (uint32_t)(p - value);
    ++p;
    if (counters_equals(name, name_length, "fmt")) {
      cell->fmt = value;
      cell->fmt_length = value_length;
    } else if (counters_equals(name, name_length, "id") ||
               counters_equals(name, name_length, "ref")) {
      const float64_t number = counters_number(value, value_length);
      if (!vkr_is_finite_f64(number) || number < 0.0) {
        return false_v;
      }
      cell->id = (uint64_t)number;
      cell->ref = name_length == 3u;
    }
  }
  if (!empty) {
    /* Text, or nested elements, then the closing tag. */
    p = counters_skip(p, end);
    if (p < end && *p != '<') {
      cell->text = p;
      while (p < end && *p != '<') {
        ++p;
      }
      cell->text_length = (uint32_t)(p - cell->text);
    }
    while (p + 1 < end && *p == '<' && p[1] != '/') {
      CountersCell child;
      if (!counters_element(ci, &p, end, &child, depth + 1u)) {
        return false_v;
      }
      if (!cell->text && child.text) {
        cell->text = child.text;
        cell->text_length = child.text_length;
      }
      p = counters_skip(p, end);
    }
    if (p + 2u + tag_length >= end || p[0] != '<' || p[1] != '/' ||
        MemCompare(p + 2, tag, tag_length) != 0 || p[2 + tag_length] != '>') {
      return false_v;
    }
    p += 3u + tag_length;
  }
  counters_register(ci, cell);
  *at = p;
  return true_v;
}

/* A cell's number, flags and text, through its reference when it has one. */
vkr_internal float64_t counters_value(const CountersImport *ci,
                                      const CountersCell *cell,
                                      uint8_t *out_flags) {
  *out_flags = 0u;
  if (cell->id == UINT64_MAX || cell->id >= ci->id_capacity) {
    return cell->ref ? VKR_FLOAT_NAN
                     : counters_number(cell->text, cell->text_length);
  }
  *out_flags = ci->flags[cell->id];
  return ci->values[cell->id];
}

vkr_internal const char *counters_text(const CountersImport *ci,
                                       const CountersCell *cell) {
  if (ci->texts && cell->id < COUNTERS_TEXT_IDS) {
    return ci->texts[cell->id];
  }
  return "";
}

/* Finds the columns of interest in the schema before the first row. */
vkr_internal void counters_header(CountersImport *ci, const uint8_t *begin,
                                  const uint8_t *end) {
  ci->header_done = true_v;
  ci->column_count = 0u;
  ci->col_time = ci->col_name = ci->col_subsystem = ci->col_message =
      UINT32_MAX;
  ci->col_counter = ci->col_type = ci->col_value = UINT32_MAX;
  static const char open[] = "<mnemonic>";
  const uint64_t open_length = sizeof(open) - 1u;
  for (const uint8_t *p = begin; p + open_length < end; ++p) {
    if (MemCompare(p, open, open_length) != 0) {
      continue;
    }
    const uint8_t *name = p + open_length;
    const uint8_t *close = name;
    while (close < end && *close != '<') {
      ++close;
    }
    const uint32_t length = (uint32_t)(close - name);
    const uint32_t column = ci->column_count++;
    if (column >= COUNTERS_COLUMN_MAX) {
      break;
    }
    const bool8_t values = ci->table == VKR_RECORDING_COUNTERS_TABLE_VALUES;
    const bool8_t signpost = ci->table == VKR_RECORDING_COUNTERS_TABLE_SIGNPOST;
    if (counters_equals(name, length, signpost ? "time" : "timestamp")) {
      ci->col_time = column;
    } else if (counters_equals(name, length, "name")) {
      ci->col_name = column;
    } else if (counters_equals(name, length, "subsystem")) {
      ci->col_subsystem = column;
    } else if (counters_equals(name, length, "message")) {
      ci->col_message = column;
    } else if (counters_equals(name, length, "counter-id")) {
      ci->col_counter = column;
    } else if (counters_equals(name, length, "type")) {
      ci->col_type = column;
    } else if (values && counters_equals(name, length, "value")) {
      ci->col_value = column;
    }
    p = close;
  }
  ci->column_count = Min(ci->column_count, COUNTERS_COLUMN_MAX);
}

vkr_internal void counters_signpost_row(CountersImport *ci,
                                        const CountersCell *cells,
                                        uint32_t count) {
  if (ci->col_time >= count || ci->col_name >= count ||
      ci->col_subsystem >= count || ci->col_message >= count) {
    return;
  }
  uint8_t name_flags = 0u;
  uint8_t subsystem_flags = 0u;
  uint8_t flags = 0u;
  (void)counters_value(ci, &cells[ci->col_name], &name_flags);
  (void)counters_value(ci, &cells[ci->col_subsystem], &subsystem_flags);
  if (!(name_flags & COUNTERS_ID_CLOCK) ||
      !(subsystem_flags & COUNTERS_ID_SUBSYSTEM)) {
    return;
  }
  const float64_t time_ns = counters_value(ci, &cells[ci->col_time], &flags);
  const float64_t clock_ns =
      counters_value(ci, &cells[ci->col_message], &flags);
  if (!vkr_is_finite_f64(time_ns) || !vkr_is_finite_f64(clock_ns)) {
    return;
  }
  if (ci->offset_count == ci->offset_capacity) {
    const uint32_t grown = Max(ci->offset_capacity * 2u, 1024u);
    int64_t *offsets =
        vkr_heap_realloc(ci->offsets, sizeof(*offsets) * (uint64_t)grown);
    if (!offsets) {
      counters_error(ci, "out of memory reading the Clock signposts");
      return;
    }
    ci->offsets = offsets;
    ci->offset_capacity = grown;
  }
  ci->offsets[ci->offset_count++] = (int64_t)clock_ns - (int64_t)time_ns;
}

vkr_internal void counters_info_row(CountersImport *ci,
                                    const CountersCell *cells, uint32_t count) {
  if (ci->col_counter >= count || ci->col_name >= count) {
    return;
  }
  uint8_t flags = 0u;
  const float64_t id = counters_value(ci, &cells[ci->col_counter], &flags);
  if (!vkr_is_finite_f64(id) || id < 0.0 || id >= COUNTERS_ID_SLOTS ||
      ci->slot_of[(uint32_t)id] != UINT8_MAX ||
      ci->counter_count >= VKR_RECORDING_GPU_COUNTER_MAX) {
    return;
  }
  const char *name = counters_text(ci, &cells[ci->col_name]);
  const char *type =
      ci->col_type < count ? counters_text(ci, &cells[ci->col_type]) : "";
  if (!name[0]) {
    return;
  }
  /* In id order, which the values table's lookups do not need but the
     file keeps. */
  uint32_t at = ci->counter_count++;
  while (at > 0u && ci->counters[at - 1u].id > (uint32_t)id) {
    ci->counters[at] = ci->counters[at - 1u];
    --at;
  }
  CountersCounter *counter = &ci->counters[at];
  *counter = (CountersCounter){.id = (uint32_t)id};
  string_format(counter->name, sizeof(counter->name), "%s", name);
  counter->unit = string_equals(type, "Percentage")
                      ? VKR_RECORDING_GPU_COUNTER_PERCENT
                  : string_contains(name, "Bandwidth")
                      ? VKR_RECORDING_GPU_COUNTER_GB_PER_SECOND
                      : VKR_RECORDING_GPU_COUNTER_COUNT;
  ci->slot_of[(uint32_t)id] = 0u;
}

vkr_internal void counters_value_row(CountersImport *ci,
                                     const CountersCell *cells,
                                     uint32_t count) {
  if (ci->col_time >= count || ci->col_counter >= count ||
      ci->col_value >= count) {
    return;
  }
  uint8_t flags = 0u;
  const float64_t id = counters_value(ci, &cells[ci->col_counter], &flags);
  const float64_t time_ns = counters_value(ci, &cells[ci->col_time], &flags);
  const float64_t value = counters_value(ci, &cells[ci->col_value], &flags);
  if (!vkr_is_finite_f64(id) || id < 0.0 || id >= COUNTERS_ID_SLOTS ||
      ci->slot_of[(uint32_t)id] == UINT8_MAX || !vkr_is_finite_f64(time_ns) ||
      time_ns < 0.0 || !vkr_is_finite_f64(value)) {
    return;
  }
  const uint64_t bucket = (uint64_t)time_ns / ci->bucket_ns;
  if (bucket >= VKR_RECORDING_GPU_COUNTER_BUCKET_MAX) {
    ci->too_long = true_v;
    return;
  }
  CountersCounter *counter = &ci->counters[ci->slot_of[(uint32_t)id]];
  if (bucket >= counter->capacity) {
    const uint64_t grown =
        Min(Max(Max(counter->capacity * 2u, bucket + 1u), (uint64_t)16384u),
            (uint64_t)VKR_RECORDING_GPU_COUNTER_BUCKET_MAX);
    float64_t *sums = vkr_heap_realloc(counter->sums, sizeof(*sums) * grown);
    if (!sums) {
      counters_error(ci, "out of memory reading the counters");
      return;
    }
    counter->sums = sums;
    uint32_t *samples =
        vkr_heap_realloc(counter->samples, sizeof(*samples) * grown);
    if (!samples) {
      counters_error(ci, "out of memory reading the counters");
      return;
    }
    counter->samples = samples;
    MemZero(counter->sums + counter->capacity,
            sizeof(*sums) * (grown - counter->capacity));
    MemZero(counter->samples + counter->capacity,
            sizeof(*samples) * (grown - counter->capacity));
    counter->capacity = grown;
  }
  counter->sums[bucket] += value;
  ++counter->samples[bucket];
  ci->last_bucket =
      ci->last_bucket == UINT64_MAX ? bucket : Max(ci->last_bucket, bucket);
  if ((++ci->out->samples & 0xFFFFu) == 0u && ci->config->samples_read) {
    vkr_atomic_uint64_store(ci->config->samples_read, ci->out->samples,
                            VKR_MEMORY_ORDER_RELAXED);
  }
}

/* Parses one row's cells and hands them to the table's reader. */
vkr_internal void counters_row(CountersImport *ci, const uint8_t *begin,
                               const uint8_t *end) {
  CountersCell cells[COUNTERS_COLUMN_MAX];
  uint32_t count = 0u;
  const uint8_t *at = begin;
  while (count < ci->column_count) {
    at = counters_skip(at, end);
    if (at >= end) {
      break;
    }
    if (!counters_element(ci, &at, end, &cells[count], 0u)) {
      counters_error(ci, "xctrace wrote a row this import cannot read");
      return;
    }
    ++count;
  }
  switch (ci->table) {
  case VKR_RECORDING_COUNTERS_TABLE_SIGNPOST:
    counters_signpost_row(ci, cells, count);
    break;
  case VKR_RECORDING_COUNTERS_TABLE_INFO:
    counters_info_row(ci, cells, count);
    break;
  default:
    counters_value_row(ci, cells, count);
    break;
  }
}

/* The first `needle` in [from, end), or NULL. */
vkr_internal const uint8_t *
counters_find(const uint8_t *from, const uint8_t *end, const char *needle) {
  const uint64_t length = string_length(needle);
  for (const uint8_t *p = from; p + length <= end; ++p) {
    if (*p == (uint8_t)needle[0] && MemCompare(p, needle, length) == 0) {
      return p;
    }
  }
  return NULL;
}

/* Parses the whole rows the buffer holds and keeps the rest. */
vkr_internal void counters_parse(CountersImport *ci, bool8_t finished) {
  const uint8_t *begin = ci->buffer;
  const uint8_t *end = ci->buffer + ci->length;
  const uint8_t *at = begin;
  if (!ci->header_done) {
    const uint8_t *row = counters_find(at, end, "<row>");
    if (!row && !finished) {
      return;
    }
    counters_header(ci, at, row ? row : end);
    at = row ? row : end;
  }
  while (!ci->failed && !ci->stopped) {
    const uint8_t *row = counters_find(at, end, "<row>");
    if (!row) {
      /* A row's start may be cut at the chunk's end. */
      at = end - Min((uint64_t)(end - at), (uint64_t)4u);
      break;
    }
    const uint8_t *close = counters_find(row + 5, end, "</row>");
    if (!close) {
      at = row;
      if ((uint64_t)(end - row) > COUNTERS_ROW_MAX) {
        counters_error(ci, "xctrace wrote a row longer than 1 MB");
      }
      break;
    }
    counters_row(ci, row + 5, close);
    at = close + 6;
  }
  const uint64_t kept = (uint64_t)(end - at);
  if (kept > 0u && at != begin) {
    MemCopy(ci->buffer, at, kept);
  }
  ci->length = kept;
}

vkr_internal bool8_t counters_sink(void *context, const uint8_t *bytes,
                                   uint64_t size) {
  CountersImport *ci = context;
  if (ci->failed || ci->stopped) {
    return false_v;
  }
  if (ci->config->is_cancelled &&
      ci->config->is_cancelled(ci->config->cancel_context)) {
    ci->stopped = true_v;
    return false_v;
  }
  if (ci->length + size > ci->capacity) {
    const uint64_t grown = Max(ci->capacity * 2u, ci->length + size + 65536u);
    uint8_t *buffer = vkr_heap_realloc(ci->buffer, grown);
    if (!buffer) {
      counters_error(ci, "out of memory reading the trace");
      return false_v;
    }
    ci->buffer = buffer;
    ci->capacity = grown;
  }
  MemCopy(ci->buffer + ci->length, bytes, size);
  ci->length += size;
  counters_parse(ci, false_v);
  return !ci->failed;
}

vkr_internal bool8_t counters_cancelled(void *context) {
  const CountersImport *ci = context;
  return ci->config->is_cancelled &&
         ci->config->is_cancelled(ci->config->cancel_context);
}

/* Runs `xcrun xctrace export` of one table into `sink`. */
vkr_internal bool8_t counters_xctrace_export(void *context,
                                             VkrRecordingCountersTable table,
                                             VkrRecordingCountersSink sink,
                                             void *sink_context) {
  CountersImport *ci = context;
  char xpath[256];
  string_format(xpath, sizeof(xpath), COUNTERS_XPATH, COUNTERS_SCHEMA[table]);
  const char *const arguments[] = {
      "xctrace", "export", "--input", ci->config->trace_path, "--xpath", xpath};
  const VkrPlatformProcessConfig process = {
      .executable = COUNTERS_XCRUN,
      .arguments = arguments,
      .argument_count = ArrayCount(arguments),
      .termination_grace_ms = 2000u,
      .is_cancelled = counters_cancelled,
      .cancel_context = ci,
      .on_stdout = sink,
      .stdout_context = sink_context,
  };
  int32_t exit_code = -1;
  bool8_t timed_out = false_v;
  if (!vkr_platform_process_run(&process, &exit_code, &timed_out)) {
    counters_error(ci, "xcrun xctrace could not run (macOS with Xcode only)");
    return false_v;
  }
  if (exit_code != 0 && !ci->failed && !ci->stopped) {
    char message[96];
    string_format(message, sizeof(message),
                  "xctrace export of %s exited with %d", COUNTERS_SCHEMA[table],
                  exit_code);
    counters_error(ci, message);
    return false_v;
  }
  return !ci->failed;
}

/* Reads one table of the trace through the source. */
vkr_internal bool8_t counters_read_table(CountersImport *ci,
                                         VkrRecordingCountersTable table) {
  ci->table = table;
  ci->length = 0u;
  ci->header_done = false_v;
  /* Ids count again from one in every export. */
  for (uint64_t i = 0u; i < ci->id_used; ++i) {
    ci->values[i] = VKR_FLOAT_NAN;
    ci->flags[i] = 0u;
  }
  ci->id_used = 0u;
  const VkrRecordingCountersImport *config = ci->config;
  const bool8_t ok =
      config->source
          ? config->source(config->source_context, table, counters_sink, ci)
          : counters_xctrace_export(ci, table, counters_sink, ci);
  if (ci->stopped) {
    counters_error(ci, "cancelled");
    return false_v;
  }
  if (!ok || ci->failed) {
    counters_error(ci, "the trace could not be read");
    return false_v;
  }
  counters_parse(ci, true_v);
  return !ci->failed;
}

vkr_internal int32_t counters_compare_i64(const void *lhs, const void *rhs) {
  const int64_t a = *(const int64_t *)lhs;
  const int64_t b = *(const int64_t *)rhs;
  return a < b ? -1 : (a > b ? 1 : 0);
}

vkr_internal bool8_t counters_write_all(FileHandle *file, const uint8_t *bytes,
                                        uint64_t size) {
  uint64_t written = 0u;
  return size == 0u ||
         (file_write(file, size, bytes, &written) == FILE_ERROR_NONE &&
          written == size);
}

/* Writes the buckets from the first with a sample to the last, beside the
   bundle's stream, through a temporary file. */
vkr_internal bool8_t counters_write(CountersImport *ci, int64_t offset_ns) {
  uint64_t first = ci->last_bucket;
  for (uint32_t c = 0u; c < ci->counter_count; ++c) {
    const CountersCounter *counter = &ci->counters[c];
    for (uint64_t b = 0u; b < Min(counter->capacity, first); ++b) {
      if (counter->samples[b] > 0u) {
        first = b;
        break;
      }
    }
  }
  const uint64_t buckets = ci->last_bucket - first + 1u;
  uint8_t header[VKR_RECORDING_GPU_COUNTERS_HEADER_SIZE] = {0};
  MemCopy(header, VKR_RECORDING_GPU_COUNTERS_MAGIC, 8u);
  vkr_store_le_u32(header + 8u, VKR_RECORDING_GPU_COUNTERS_VERSION);
  vkr_store_le_u32(header + 12u, ci->counter_count);
  vkr_store_le_u32(header + 16u, (uint32_t)buckets);
  vkr_store_le_u32(header + 20u, (uint32_t)ci->bucket_ns);
  vkr_store_le_u64(header + 24u,
                   (uint64_t)(offset_ns + (int64_t)(first * ci->bucket_ns)));
  vkr_store_le_u32(header + 32u, ci->offset_count);
  vkr_store_le_u32(header + 36u,
                   (uint32_t)Min(ci->out->spread_ns, (uint64_t)UINT32_MAX));
  uint8_t names[VKR_RECORDING_GPU_COUNTER_MAX *
                    (2u + VKR_RECORDING_GPU_COUNTER_NAME_MAX) +
                4u] = {0};
  uint64_t names_size = 0u;
  for (uint32_t c = 0u; c < ci->counter_count; ++c) {
    const uint32_t length = (uint32_t)string_length(ci->counters[c].name);
    names[names_size] = ci->counters[c].unit;
    names[names_size + 1u] = (uint8_t)length;
    MemCopy(names + names_size + 2u, ci->counters[c].name, length);
    names_size += 2u + length;
  }
  names_size = vkr_align_up_u64(
                   VKR_RECORDING_GPU_COUNTERS_HEADER_SIZE + names_size, 4u) -
               VKR_RECORDING_GPU_COUNTERS_HEADER_SIZE;

  char path[COUNTERS_PATH_MAX];
  char temp[COUNTERS_PATH_MAX];
  if (string_format(path, sizeof(path), "%s/%s", ci->config->bundle_path,
                    VKR_RECORDING_GPU_COUNTERS_FILE) >= (int32_t)sizeof(path) ||
      string_format(temp, sizeof(temp), "%s.tmp", path) >=
          (int32_t)sizeof(temp)) {
    counters_error(ci, "the recording's path is too long");
    return false_v;
  }
  uint8_t *values = vkr_heap_alloc(sizeof(float32_t) * buckets);
  if (!values) {
    counters_error(ci, "out of memory writing the counters");
    return false_v;
  }
  const FilePath temp_path = file_path_view(temp);
  FileHandle file = {0};
  bool8_t ok =
      file_open(&temp_path,
                (FileMode){.set = FILE_MODE_WRITE | FILE_MODE_CREATE |
                                  FILE_MODE_TRUNCATE | FILE_MODE_BINARY},
                &file) == FILE_ERROR_NONE;
  const bool8_t opened = ok;
  ok = ok && counters_write_all(&file, header, sizeof(header)) &&
       counters_write_all(&file, names, names_size);
  for (uint32_t c = 0u; ok && c < ci->counter_count; ++c) {
    const CountersCounter *counter = &ci->counters[c];
    for (uint64_t b = 0u; b < buckets; ++b) {
      const uint64_t bucket = first + b;
      const bool8_t sampled =
          bucket < counter->capacity && counter->samples[bucket] > 0u;
      vkr_store_le_f32(values + b * 4u,
                       sampled ? (float32_t)(counter->sums[bucket] /
                                             counter->samples[bucket])
                               : (float32_t)VKR_FLOAT_NAN);
    }
    ok = counters_write_all(&file, values, sizeof(float32_t) * buckets);
  }
  if (opened) {
    file_close(&file);
  }
  vkr_heap_free(values);
  const FilePath final_path = file_path_view(path);
  if (!ok || file_rename(&temp_path, &final_path, true_v) != FILE_ERROR_NONE) {
    (void)file_remove(&temp_path);
    counters_error(ci, "the counter file could not be written into the "
                       "recording");
    return false_v;
  }
  ci->out->buckets = (uint32_t)buckets;
  ci->out->file_bytes =
      sizeof(header) + names_size + (uint64_t)ci->counter_count * buckets * 4u;
  return true_v;
}

vkr_internal void counters_release(CountersImport *ci) {
  for (uint32_t c = 0u; c < ci->counter_count; ++c) {
    vkr_heap_free(ci->counters[c].sums);
    vkr_heap_free(ci->counters[c].samples);
  }
  vkr_heap_free(ci->buffer);
  vkr_heap_free(ci->values);
  vkr_heap_free(ci->flags);
  vkr_heap_free(ci->texts);
  vkr_heap_free(ci->offsets);
}

/* The three tables in turn, then the file. */
vkr_internal bool8_t counters_run(CountersImport *ci) {
  if (!counters_read_table(ci, VKR_RECORDING_COUNTERS_TABLE_SIGNPOST)) {
    return false_v;
  }
  if (ci->offset_count == 0u) {
    counters_error(ci, "the trace has no Clock signposts: trace the "
                       "os_signpost instrument of a run with signposts on");
    return false_v;
  }
  vkr_sort(ci->offsets, ci->offset_count, sizeof(*ci->offsets),
           counters_compare_i64);
  const int64_t offset_ns = ci->offsets[ci->offset_count / 2u];
  ci->out->anchors = ci->offset_count;
  ci->out->spread_ns =
      (uint64_t)(ci->offsets[ci->offset_count - 1u] - ci->offsets[0]);

  ci->texts = vkr_heap_alloc_zeroed(COUNTERS_TEXT_IDS, sizeof(*ci->texts));
  if (!ci->texts) {
    counters_error(ci, "out of memory reading the counters");
    return false_v;
  }
  if (!counters_read_table(ci, VKR_RECORDING_COUNTERS_TABLE_INFO)) {
    return false_v;
  }
  vkr_heap_free(ci->texts);
  ci->texts = NULL;
  if (ci->counter_count == 0u) {
    counters_error(ci, "the trace has no GPU counters: trace the Metal GPU "
                       "Counters instrument");
    return false_v;
  }
  for (uint32_t c = 0u; c < ci->counter_count; ++c) {
    ci->slot_of[ci->counters[c].id] = (uint8_t)c;
  }
  ci->out->counters = ci->counter_count;

  if (!counters_read_table(ci, VKR_RECORDING_COUNTERS_TABLE_VALUES)) {
    return false_v;
  }
  if (ci->config->samples_read) {
    vkr_atomic_uint64_store(ci->config->samples_read, ci->out->samples,
                            VKR_MEMORY_ORDER_RELAXED);
  }
  if (ci->too_long) {
    counters_error(ci, "the trace is longer than a counter file holds; "
                       "trace up to 10 s");
    return false_v;
  }
  if (ci->last_bucket == UINT64_MAX) {
    counters_error(ci, "the trace has no GPU counter samples");
    return false_v;
  }
  return counters_write(ci, offset_ns);
}

bool8_t vkr_recording_counters_import(const VkrRecordingCountersImport *import,
                                      VkrRecordingCountersResult *out) {
  vkr_assert(import != NULL && out != NULL);
  MemZero(out, sizeof(*out));
  if (!import->bundle_path || (!import->trace_path && !import->source)) {
    string_format(out->error, sizeof(out->error),
                  "a trace and a recording bundle are needed");
    return false_v;
  }
  const uint32_t bucket_us =
      import->bucket_us ? import->bucket_us : VKR_RECORDING_COUNTERS_BUCKET_US;
  if (bucket_us < 10u || bucket_us > 10000u) {
    string_format(out->error, sizeof(out->error),
                  "the bucket lies in [10, 10000] us");
    return false_v;
  }
  CountersImport *ci = vkr_heap_alloc_zeroed(1u, sizeof(*ci));
  if (!ci) {
    string_format(out->error, sizeof(out->error), "out of memory");
    return false_v;
  }
  ci->config = import;
  ci->out = out;
  ci->bucket_ns = (uint64_t)bucket_us * 1000u;
  ci->last_bucket = UINT64_MAX;
  MemSet(ci->slot_of, UINT8_MAX, sizeof(ci->slot_of));
  out->bucket_ns = (uint32_t)ci->bucket_ns;
  const bool8_t ok = counters_grow_ids(ci, 0u) && counters_run(ci);
  if (!ok && !out->error[0]) {
    string_format(out->error, sizeof(out->error), "out of memory");
  }
  counters_release(ci);
  vkr_heap_free(ci);
  return ok;
}

bool8_t vkr_recording_counters_trace(uint32_t pid, uint32_t seconds,
                                     const char *trace_path,
                                     bool8_t (*is_cancelled)(void *context),
                                     void *cancel_context, char *error,
                                     uint64_t error_capacity) {
  vkr_assert(trace_path != NULL && error != NULL && error_capacity > 0u);
  error[0] = '\0';
  if (seconds == 0u || seconds > VKR_RECORDING_COUNTERS_TRACE_MAX_S) {
    string_format(error, error_capacity, "a trace lasts 1 to %u s",
                  VKR_RECORDING_COUNTERS_TRACE_MAX_S);
    return false_v;
  }
  char process[16];
  char limit[16];
  char log[COUNTERS_PATH_MAX];
  string_format(process, sizeof(process), "%u", pid);
  string_format(limit, sizeof(limit), "%us", seconds);
  if (string_format(log, sizeof(log), "%s.log", trace_path) >=
      (int32_t)sizeof(log)) {
    string_format(error, error_capacity, "the trace path is too long");
    return false_v;
  }
  const char *const arguments[] = {
      "xctrace",      "record",      "--instrument", "Metal GPU Counters",
      "--instrument", "os_signpost", "--attach",     process,
      "--time-limit", limit,         "--output",     trace_path,
  };
  /* xctrace ends the trace itself at its limit; the timeout covers its
     start and save. */
  const VkrPlatformProcessConfig config = {
      .executable = COUNTERS_XCRUN,
      .arguments = arguments,
      .argument_count = ArrayCount(arguments),
      .stdout_path = log,
      .stderr_path = log,
      .append_output = true_v,
      .timeout_ms = (seconds + 120u) * 1000u,
      .termination_grace_ms = 5000u,
      .is_cancelled = is_cancelled,
      .cancel_context = cancel_context,
  };
  int32_t exit_code = -1;
  bool8_t timed_out = false_v;
  const bool8_t ran = vkr_platform_process_run(&config, &exit_code, &timed_out);
  const FilePath log_path = file_path_view(log);
  const FilePath trace = file_path_view(trace_path);
  if (!ran) {
    string_format(error, error_capacity,
                  "xcrun xctrace could not run (macOS with Xcode only)");
  } else if (is_cancelled && is_cancelled(cancel_context)) {
    string_format(error, error_capacity, "cancelled");
  } else if (timed_out) {
    string_format(error, error_capacity, "xctrace did not finish in time");
  } else if (exit_code != 0 || !file_exists(&trace)) {
    string_format(error, error_capacity,
                  "xctrace record exited with %d; its output is in %s",
                  exit_code, log);
    return false_v;
  }
  (void)file_remove(&log_path);
  return error[0] == '\0';
}

typedef struct CountersRemove {
  const char *directory;
  uint32_t depth;
} CountersRemove;

vkr_internal bool8_t counters_remove_tree(const char *directory,
                                          uint32_t depth);

vkr_internal bool8_t counters_remove_visit(void *context, const char *name,
                                           bool8_t is_directory) {
  const CountersRemove *remove = context;
  char path[COUNTERS_PATH_MAX];
  if (string_format(path, sizeof(path), "%s/%s", remove->directory, name) >=
      (int32_t)sizeof(path)) {
    return true_v;
  }
  if (is_directory) {
    (void)counters_remove_tree(path, remove->depth + 1u);
    return true_v;
  }
  const FilePath file = file_path_view(path);
  (void)file_remove(&file);
  return true_v;
}

/* A trace bundle nests a few directories; deeper ones are left. */
vkr_internal bool8_t counters_remove_tree(const char *directory,
                                          uint32_t depth) {
  if (depth > 12u) {
    return false_v;
  }
  CountersRemove remove = {.directory = directory, .depth = depth};
  const FilePath path = file_path_view(directory);
  (void)file_list_directory(&path, counters_remove_visit, &remove);
  return file_remove_directory(&path) == FILE_ERROR_NONE;
}

bool8_t vkr_recording_counters_remove_trace(const char *trace_path) {
  const uint64_t length = trace_path ? string_length(trace_path) : 0u;
  const uint64_t suffix = string_length(".trace");
  if (length <= suffix ||
      !string_equals(trace_path + length - suffix, ".trace")) {
    return false_v;
  }
  return counters_remove_tree(trace_path, 0u);
}
