#pragma once

#include "vkr_harness.h"

#define VKR_HARNESS_JSON_MAX_TOKENS 4096u

typedef enum VkrHarnessJsonTokenType {
  VKR_HARNESS_JSON_UNDEFINED = 0,
  VKR_HARNESS_JSON_OBJECT,
  VKR_HARNESS_JSON_ARRAY,
  VKR_HARNESS_JSON_STRING,
  VKR_HARNESS_JSON_NUMBER,
  VKR_HARNESS_JSON_BOOL,
  VKR_HARNESS_JSON_NULL,
} VkrHarnessJsonTokenType;

typedef struct VkrHarnessJsonToken {
  VkrHarnessJsonTokenType type;
  uint32_t start;
  uint32_t end;
  int32_t parent;
  uint32_t child_count;
} VkrHarnessJsonToken;

typedef struct VkrHarnessJsonDocument {
  const char *json;
  uint64_t length;
  VkrHarnessJsonToken tokens[VKR_HARNESS_JSON_MAX_TOKENS];
  uint32_t token_count;
} VkrHarnessJsonDocument;

bool8_t vkr_harness_json_parse(VkrHarnessJsonDocument *document,
                               const char *json, uint64_t length,
                               VkrHarnessError *out_error);
int32_t vkr_harness_json_object_get(const VkrHarnessJsonDocument *document,
                                    int32_t object_token, const char *name,
                                    bool8_t *out_duplicate);
bool8_t vkr_harness_json_object_validate(
    const VkrHarnessJsonDocument *document, int32_t object_token,
    const char *const *allowed, uint32_t allowed_count,
    const char *const *required, uint32_t required_count, const char *field,
    VkrHarnessError *out_error);
int32_t vkr_harness_json_next(const VkrHarnessJsonDocument *document,
                              int32_t token);
bool8_t vkr_harness_json_string(const VkrHarnessJsonDocument *document,
                                int32_t token, char *out, uint32_t out_capacity,
                                const char *field, VkrHarnessError *out_error);
bool8_t vkr_harness_json_u64(const VkrHarnessJsonDocument *document,
                             int32_t token, uint64_t *out, const char *field,
                             VkrHarnessError *out_error);
bool8_t vkr_harness_json_f64(const VkrHarnessJsonDocument *document,
                             int32_t token, float64_t *out, const char *field,
                             VkrHarnessError *out_error);
bool8_t vkr_harness_json_bool(const VkrHarnessJsonDocument *document,
                              int32_t token, bool8_t *out, const char *field,
                              VkrHarnessError *out_error);
