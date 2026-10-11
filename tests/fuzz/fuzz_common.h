#pragma once

#include "defines.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shared pieces of the transport and depot fuzzers (ADR-105, "Security").
 * Each fuzzer is one libFuzzer target, or a replay driver over input files
 * when the compiler has no libFuzzer (fuzz_replay_main.c). A fuzzer writes
 * its seed corpus from code when VKR_FUZZ_SEED_DIR names a directory, then
 * exits, so no binary corpus lives in the repository.
 *
 * Checks abort in every configuration: an oracle that a release build
 * compiles out finds nothing. */

#define FUZZ_CHECK(condition)                                                  \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "fuzz check failed: %s (%s:%d)\n", #condition, __FILE__, \
              __LINE__);                                                       \
      abort();                                                                 \
    }                                                                          \
  } while (0)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerInitialize(int *argc, char ***argv);

// =============================================================================
// Input cursor
// =============================================================================

/* Reads fields from the front of a fuzz input; past the end every read
   yields zero and an empty view. */
typedef struct FuzzInput {
  const uint8_t *data;
  uint32_t size;
  uint32_t offset;
} FuzzInput;

static inline FuzzInput fuzz_input(const uint8_t *data, size_t size) {
  const uint32_t clamped = size > UINT32_MAX ? UINT32_MAX : (uint32_t)size;
  return (FuzzInput){.data = data, .size = clamped};
}

static inline uint32_t fuzz_remaining(const FuzzInput *input) {
  return input->size - input->offset;
}

static inline uint8_t fuzz_u8(FuzzInput *input) {
  if (input->offset >= input->size) {
    return 0u;
  }
  return input->data[input->offset++];
}

static inline uint16_t fuzz_u16(FuzzInput *input) {
  const uint16_t low = fuzz_u8(input);
  const uint16_t high = fuzz_u8(input);
  return (uint16_t)(low | (high << 8));
}

static inline uint32_t fuzz_u32(FuzzInput *input) {
  const uint32_t low = fuzz_u16(input);
  const uint32_t high = fuzz_u16(input);
  return low | (high << 16);
}

/* A view of up to `size` bytes; `out_size` holds how many were left. */
static inline const uint8_t *fuzz_bytes(FuzzInput *input, uint32_t size,
                                        uint32_t *out_size) {
  const uint32_t taken = Min(size, fuzz_remaining(input));
  const uint8_t *view = input->data + input->offset;
  input->offset += taken;
  *out_size = taken;
  return view;
}

// =============================================================================
// Seed corpus
// =============================================================================

/* Builds one seed input in a fixed buffer. */
typedef struct FuzzSeed {
  uint8_t bytes[16384];
  uint32_t size;
} FuzzSeed;

static inline void fuzz_seed_u8(FuzzSeed *seed, uint8_t value) {
  FUZZ_CHECK(seed->size < sizeof(seed->bytes));
  seed->bytes[seed->size++] = value;
}

static inline void fuzz_seed_u16(FuzzSeed *seed, uint16_t value) {
  fuzz_seed_u8(seed, (uint8_t)(value & 0xffu));
  fuzz_seed_u8(seed, (uint8_t)(value >> 8));
}

static inline void fuzz_seed_u32(FuzzSeed *seed, uint32_t value) {
  fuzz_seed_u16(seed, (uint16_t)(value & 0xffffu));
  fuzz_seed_u16(seed, (uint16_t)(value >> 16));
}

static inline void fuzz_seed_bytes(FuzzSeed *seed, const void *bytes,
                                   uint32_t size) {
  FUZZ_CHECK(size <= sizeof(seed->bytes) - seed->size);
  MemCopy(seed->bytes + seed->size, bytes, size);
  seed->size += size;
}

/* An operation record: an opcode, a 16-bit body length and the body. */
static inline void fuzz_seed_record(FuzzSeed *seed, uint8_t op,
                                    const void *body, uint32_t size) {
  FUZZ_CHECK(size <= 0xffffu);
  fuzz_seed_u8(seed, op);
  fuzz_seed_u16(seed, (uint16_t)size);
  fuzz_seed_bytes(seed, body, size);
}

typedef struct FuzzSeedWriter {
  const char *directory;
  uint32_t count;
} FuzzSeedWriter;

static inline void fuzz_seed_write(FuzzSeedWriter *writer, const void *bytes,
                                   uint32_t size) {
  char path[4096];
  snprintf(path, sizeof(path), "%s/seed_%03u", writer->directory,
           writer->count++);
  FILE *file = fopen(path, "wb");
  FUZZ_CHECK(file);
  FUZZ_CHECK(size == 0u || fwrite(bytes, 1u, size, file) == size);
  FUZZ_CHECK(fclose(file) == 0);
}

static inline void fuzz_seed_emit(FuzzSeedWriter *writer,
                                  const FuzzSeed *seed) {
  fuzz_seed_write(writer, seed->bytes, seed->size);
}

/* Writes the seed corpus and exits when VKR_FUZZ_SEED_DIR is set. */
static inline void fuzz_seed_mode(void (*write_seeds)(FuzzSeedWriter *)) {
  const char *directory = getenv("VKR_FUZZ_SEED_DIR");
  if (!directory || !directory[0]) {
    return;
  }
  FuzzSeedWriter writer = {.directory = directory};
  write_seeds(&writer);
  fprintf(stderr, "wrote %u seeds to %s\n", writer.count, directory);
  exit(0);
}
