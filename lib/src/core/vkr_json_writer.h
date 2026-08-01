/**
 * @file vkr_json_writer.h
 * @brief Bounded, emit-only JSON writer with optional atomic file publication.
 */
#pragma once

#include "containers/str.h"
#include "defines.h"

#define VKR_JSON_WRITER_MAX_DEPTH 32u
#define VKR_JSON_WRITER_BUFFER_SIZE 512u
#define VKR_JSON_WRITER_PATH_MAX 1023u

typedef bool8_t (*VkrJsonWriteSink)(void *context, const uint8_t *data,
                                    uint64_t length);

typedef enum VkrJsonContainerKind {
  VKR_JSON_CONTAINER_OBJECT,
  VKR_JSON_CONTAINER_ARRAY,
} VkrJsonContainerKind;

typedef struct VkrJsonWriterLevel {
  VkrJsonContainerKind kind;
  uint32_t item_count;
  bool8_t expecting_value;
} VkrJsonWriterLevel;

typedef struct VkrJsonWriter {
  VkrJsonWriteSink sink;
  void *sink_context;
  VkrJsonWriterLevel levels[VKR_JSON_WRITER_MAX_DEPTH];
  uint8_t buffer[VKR_JSON_WRITER_BUFFER_SIZE];
  uint32_t buffer_count;
  uint32_t depth;
  bool8_t root_written;
  bool8_t failed;
} VkrJsonWriter;

typedef struct VkrJsonFileWriter {
  VkrJsonWriter writer;
  FILE *file;
  char final_path[VKR_JSON_WRITER_PATH_MAX + 1u];
  char temp_path[VKR_JSON_WRITER_PATH_MAX + 1u];
  bool8_t active;
} VkrJsonFileWriter;

void vkr_json_writer_init(VkrJsonWriter *writer, VkrJsonWriteSink sink,
                          void *sink_context);
bool8_t vkr_json_writer_flush(VkrJsonWriter *writer);
bool8_t vkr_json_writer_complete(VkrJsonWriter *writer);

bool8_t vkr_json_writer_begin_object(VkrJsonWriter *writer);
bool8_t vkr_json_writer_end_object(VkrJsonWriter *writer);
bool8_t vkr_json_writer_begin_array(VkrJsonWriter *writer);
bool8_t vkr_json_writer_end_array(VkrJsonWriter *writer);
bool8_t vkr_json_writer_name(VkrJsonWriter *writer, String8 name);
bool8_t vkr_json_writer_string(VkrJsonWriter *writer, String8 value);
bool8_t vkr_json_writer_u64(VkrJsonWriter *writer, uint64_t value);
bool8_t vkr_json_writer_i64(VkrJsonWriter *writer, int64_t value);
bool8_t vkr_json_writer_f64(VkrJsonWriter *writer, float64_t value);
bool8_t vkr_json_writer_bool(VkrJsonWriter *writer, bool8_t value);
bool8_t vkr_json_writer_null(VkrJsonWriter *writer);

bool8_t vkr_json_file_writer_begin(VkrJsonFileWriter *file_writer,
                                   String8 final_path);
bool8_t vkr_json_file_writer_commit(VkrJsonFileWriter *file_writer);
void vkr_json_file_writer_abort(VkrJsonFileWriter *file_writer);
