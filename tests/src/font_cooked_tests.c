#include "font_cooked_tests.h"

#include "filesystem/filesystem.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_platform.h"
#include "renderer/resources/loaders/vkr_font_cooked.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t test_font_crc32(const uint8_t *data, uint64_t size) {
  uint32_t crc = 0xffffffffu;
  for (uint64_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (uint32_t bit = 0; bit < 8u; ++bit) {
      crc = (crc >> 1u) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
  }
  return ~crc;
}

static uint32_t test_font_read_u32(const uint8_t *data) {
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
         ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
}

static uint64_t test_font_read_u64(const uint8_t *data) {
  return (uint64_t)test_font_read_u32(data) |
         ((uint64_t)test_font_read_u32(data + 4u) << 32u);
}

static void test_font_write_u32(uint8_t *data, uint32_t value) {
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8u);
  data[2] = (uint8_t)(value >> 16u);
  data[3] = (uint8_t)(value >> 24u);
}

static void test_font_write_u64(uint8_t *data, uint64_t value) {
  test_font_write_u32(data, (uint32_t)value);
  test_font_write_u32(data + 4u, (uint32_t)(value >> 32u));
}

static void test_font_write_f32(uint8_t *data, float32_t value) {
  uint32_t bits = 0u;
  MemCopy(&bits, &value, sizeof(bits));
  test_font_write_u32(data, bits);
}

static void test_font_refresh_checksums(uint8_t *data, uint64_t size) {
  for (uint32_t i = 0; i < VKR_FONT_COOKED_SECTION_COUNT; ++i) {
    uint8_t *entry = data + VKR_FONT_COOKED_DIRECTORY_OFFSET +
                     i * VKR_FONT_COOKED_SECTION_SIZE;
    const uint64_t offset = test_font_read_u64(entry + 8u);
    const uint64_t section_size = test_font_read_u64(entry + 16u);
    test_font_write_u32(entry + 24u,
                        test_font_crc32(data + offset, section_size));
  }
  test_font_write_u32(data + VKR_FONT_COOKED_DIRECTORY_CRC_OFFSET,
                      test_font_crc32(data + VKR_FONT_COOKED_DIRECTORY_OFFSET,
                                      VKR_FONT_COOKED_DIRECTORY_SIZE));
  test_font_write_u32(data + VKR_FONT_COOKED_PAYLOAD_CRC_OFFSET,
                      test_font_crc32(data + VKR_FONT_COOKED_DATA_OFFSET,
                                      size - VKR_FONT_COOKED_DATA_OFFSET));
  test_font_write_u32(data + VKR_FONT_COOKED_HEADER_CRC_OFFSET, 0u);
  test_font_write_u32(data + VKR_FONT_COOKED_HEADER_CRC_OFFSET,
                      test_font_crc32(data, VKR_FONT_COOKED_HEADER_SIZE));
}

typedef struct TestFontFixture {
  VkrFontCookedEncodeInfo info;
  VkrFontCookedGlyph glyphs[2];
  VkrFontCookedCodepoint codepoints[2];
  VkrFontCookedKerning kernings[1];
  VkrFontCookedPage pages[1];
  uint8_t pixels[16];
} TestFontFixture;

static TestFontFixture test_font_fixture(void) {
  TestFontFixture fixture = {0};
  fixture.glyphs[0] = (VkrFontCookedGlyph){
      .glyph_id = 1u,
      .page_index = 0u,
      .flags = VKR_FONT_COOKED_GLYPH_HAS_GEOMETRY,
      .advance = 0.5f,
      .plane_left = 0.0f,
      .plane_bottom = -0.2f,
      .plane_right = 0.4f,
      .plane_top = 0.8f,
      .uv_left = 0.0f,
      .uv_bottom = 0.0f,
      .uv_right = 0.5f,
      .uv_top = 0.5f,
  };
  fixture.glyphs[1] = fixture.glyphs[0];
  fixture.glyphs[1].glyph_id = 2u;
  fixture.glyphs[1].advance = 0.6f;
  fixture.glyphs[1].uv_left = 0.5f;
  fixture.glyphs[1].uv_right = 1.0f;
  fixture.codepoints[0] =
      (VkrFontCookedCodepoint){.codepoint = 0x20u, .glyph_id = 1u};
  fixture.codepoints[1] =
      (VkrFontCookedCodepoint){.codepoint = 0x41u, .glyph_id = 2u};
  fixture.kernings[0] = (VkrFontCookedKerning){
      .left_glyph_id = 1u, .right_glyph_id = 2u, .amount = -0.025f};
  for (uint32_t i = 0; i < sizeof(fixture.pixels); ++i)
    fixture.pixels[i] = (uint8_t)(i * 13u);
  fixture.pages[0] = (VkrFontCookedPage){
      .width = 2u,
      .height = 2u,
      .row_stride = 8u,
      .pixel_format = VKR_FONT_COOKED_PIXEL_RGBA8_UNORM,
      .pixels = fixture.pixels,
      .pixel_size = sizeof(fixture.pixels),
  };
  fixture.info = (VkrFontCookedEncodeInfo){
      .face = string8_lit("Frozen Test Faces"),
      .cooker_version = 7u,
      .field_kind = VKR_FONT_COOKED_FIELD_MTSDF,
      .fallback_glyph_id = 1u,
      .metrics = {.line_height = 1.2f,
                  .ascender = 0.9f,
                  .descender = -0.3f,
                  .underline_y = -0.1f,
                  .underline_thickness = 0.05f,
                  .distance_range = 8.0f,
                  .atlas_px_per_em = 64.0f,
                  .units_per_em = 1000u},
      .glyphs = fixture.glyphs,
      .glyph_count = 2u,
      .codepoints = fixture.codepoints,
      .codepoint_count = 2u,
      .kernings = fixture.kernings,
      .kerning_count = 1u,
      .pages = fixture.pages,
      .page_count = 1u,
  };
  for (uint32_t i = 0; i < sizeof(fixture.info.identity); ++i)
    fixture.info.identity[i] = (uint8_t)(0xa0u + i);
  return fixture;
}

static void test_font_cooked_round_trip_and_golden(void) {
  printf("  Running test_font_cooked_round_trip_and_golden...\n");
  TestFontFixture fixture = test_font_fixture();
  Arena *scratch_arena = arena_create(MB(2), KB(64));
  Arena *result_arena = arena_create(MB(2), KB(64));
  assert(scratch_arena && result_arena);
  VkrAllocator scratch = {.ctx = scratch_arena};
  VkrAllocator result = {.ctx = result_arena};
  assert(vkr_allocator_arena(&scratch) && vkr_allocator_arena(&result));
  uint8_t *first = NULL, *second = NULL;
  uint64_t first_size = 0u, second_size = 0u;
  assert(vkr_font_cooked_encode(&scratch, &fixture.info, &first, &first_size));
  assert(
      vkr_font_cooked_encode(&scratch, &fixture.info, &second, &second_size));
  assert(first_size == second_size &&
         MemCompare(first, second, first_size) == 0);
  assert(test_font_crc32(first, first_size) == 0xa454970au);
  assert(test_font_read_u32(first) == VKR_FONT_COOKED_MAGIC);
  assert(test_font_read_u32(first + 4u) == VKR_FONT_COOKED_VERSION);
  assert(test_font_read_u32(first + 8u) == VKR_FONT_COOKED_ENDIAN_TAG);
  assert(test_font_read_u32(first + 12u) == VKR_FONT_COOKED_HEADER_SIZE);
  assert(first_size == 608u);
  assert(test_font_read_u64(first + 48u) == first_size);
  assert(test_font_read_u32(first + 56u) == fixture.info.face.length);
  assert(first[64] == 0xa0u && first[95] == 0xbfu);
  assert(test_font_read_u32(first + 96u) == 0x3f99999au);
  const uint64_t expected_offsets[VKR_FONT_COOKED_SECTION_COUNT] = {
      384u, 416u, 512u, 528u, 544u, 592u};
  const uint64_t expected_sizes[VKR_FONT_COOKED_SECTION_COUNT] = {
      17u, 96u, 16u, 16u, 48u, 16u};
  for (uint32_t i = 0; i < VKR_FONT_COOKED_SECTION_COUNT; ++i) {
    const uint8_t *entry = first + VKR_FONT_COOKED_DIRECTORY_OFFSET +
                           i * VKR_FONT_COOKED_SECTION_SIZE;
    assert(test_font_read_u32(entry) == i + 1u);
    assert(test_font_read_u32(entry + 4u) == 0u);
    assert(test_font_read_u64(entry + 8u) == expected_offsets[i]);
    assert(test_font_read_u64(entry + 16u) == expected_sizes[i]);
    assert(test_font_read_u32(entry + 28u) == 0u);
  }
  assert(test_font_read_u32(first + 60u) == 0u);
  assert(test_font_read_u32(first + 128u) == 0u);
  for (uint32_t i = 144u; i < VKR_FONT_COOKED_HEADER_SIZE; ++i)
    assert(first[i] == 0u);
  for (uint32_t i = 401u; i < 416u; ++i)
    assert(first[i] == 0u);
  VkrFontCookedInspection inspection = {0};
  assert(vkr_font_cooked_inspect(first, first_size, &inspection));
  assert(inspection.file_size == first_size && inspection.glyph_count == 2u &&
         inspection.page_count == 1u);
  VkrFontCookedDecoded decoded = {0};
  assert(vkr_font_cooked_decode(&result, first, first_size, &decoded));
  assert(decoded.face.length == fixture.info.face.length &&
         MemCompare(decoded.face.str, fixture.info.face.str,
                    decoded.face.length) == 0);
  assert(decoded.glyphs[1].uv_left == fixture.glyphs[1].uv_left);
  assert(decoded.kernings[0].amount == fixture.kernings[0].amount);
  const uint64_t page_section_offset =
      test_font_read_u64(first + VKR_FONT_COOKED_DIRECTORY_OFFSET +
                         4u * VKR_FONT_COOKED_SECTION_SIZE + 8u);
  const uint64_t pixel_offset =
      test_font_read_u64(first + page_section_offset + 16u);
  assert(decoded.pages[0].pixels == first + pixel_offset);
  assert(decoded.pages[0].pixel_size == sizeof(fixture.pixels));
  VkrFontCookedEncodeInfo invalid = fixture.info;
  invalid.cooker_version = 0u;
  assert(!vkr_font_cooked_encode(&scratch, &invalid, &second, &second_size));
  invalid = fixture.info;
  invalid.codepoint_count = 0u;
  assert(!vkr_font_cooked_encode(&scratch, &invalid, &second, &second_size));
  vkr_allocator_release_global_accounting(&scratch);
  vkr_allocator_release_global_accounting(&result);
  arena_destroy(scratch_arena);
  arena_destroy(result_arena);
  printf("  test_font_cooked_round_trip_and_golden PASSED\n");
}

static void test_font_cooked_atomic_publication(void) {
  printf("  Running test_font_cooked_atomic_publication...\n");
  TestFontFixture fixture = test_font_fixture();
  Arena *arena = arena_create(MB(2), KB(64));
  assert(arena);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));

  uint8_t *artifact = NULL;
  uint64_t artifact_size = 0u;
  assert(vkr_font_cooked_encode(&allocator, &fixture.info, &artifact,
                                &artifact_size));
  const String8 output_text =
      string8_lit("build/vkr_font_cooked_atomic_test.vkfa");
  FilePath output = file_path_create("build/vkr_font_cooked_atomic_test.vkfa",
                                     &allocator, FILE_PATH_TYPE_RELATIVE);
  if (file_exists(&output))
    assert(file_remove(&output) == FILE_ERROR_NONE);

  String8 collision_text = string8_create_formatted(
      &allocator, "%.*s.tmp.%u.0", (int32_t)output_text.length, output_text.str,
      vkr_platform_get_process_id());
  String8 retry_text = string8_create_formatted(
      &allocator, "%.*s.tmp.%u.1", (int32_t)output_text.length, output_text.str,
      vkr_platform_get_process_id());
  FilePath collision = file_path_create(string8_cstr(&collision_text),
                                        &allocator, FILE_PATH_TYPE_RELATIVE);
  FilePath retry = file_path_create(string8_cstr(&retry_text), &allocator,
                                    FILE_PATH_TYPE_RELATIVE);
  if (file_exists(&collision))
    assert(file_remove(&collision) == FILE_ERROR_NONE);
  if (file_exists(&retry))
    assert(file_remove(&retry) == FILE_ERROR_NONE);
  FileMode collision_mode = bitset8_create();
  bitset8_set(&collision_mode, FILE_MODE_WRITE);
  bitset8_set(&collision_mode, FILE_MODE_CREATE);
  bitset8_set(&collision_mode, FILE_MODE_EXCLUSIVE);
  bitset8_set(&collision_mode, FILE_MODE_BINARY);
  FileHandle collision_file = {0};
  assert(file_open(&collision, collision_mode, &collision_file) ==
         FILE_ERROR_NONE);
  const uint8_t collision_sentinel[] = {0xc0u, 0x11u, 0x1du, 0xedu};
  uint64_t sentinel_written = 0u;
  assert(file_write(&collision_file, sizeof(collision_sentinel),
                    collision_sentinel, &sentinel_written) == FILE_ERROR_NONE);
  assert(sentinel_written == sizeof(collision_sentinel));
  file_close(&collision_file);

  assert(vkr_font_cooked_write_atomic(&allocator, output_text, artifact,
                                      artifact_size));
  FileMode read_mode = bitset8_create();
  bitset8_set(&read_mode, FILE_MODE_READ);
  bitset8_set(&read_mode, FILE_MODE_BINARY);
  FileHandle file = {0};
  uint8_t *published = NULL;
  uint64_t published_size = 0u;
  assert(file_open(&output, read_mode, &file) == FILE_ERROR_NONE);
  assert(file_read_all(&file, &allocator, &published, &published_size) ==
         FILE_ERROR_NONE);
  file_close(&file);
  assert(published_size == artifact_size &&
         MemCompare(published, artifact, artifact_size) == 0);
  assert(file_exists(&collision));
  assert(!file_exists(&retry));
  uint8_t *sentinel = NULL;
  uint64_t sentinel_size = 0u;
  assert(file_open(&collision, read_mode, &file) == FILE_ERROR_NONE);
  assert(file_read_all(&file, &allocator, &sentinel, &sentinel_size) ==
         FILE_ERROR_NONE);
  file_close(&file);
  assert(sentinel_size == sizeof(collision_sentinel) &&
         MemCompare(sentinel, collision_sentinel, sizeof(collision_sentinel)) ==
             0);

  const uint8_t replacement[] = {0x56u, 0x4bu, 0x46u, 0x41u};
  assert(vkr_font_cooked_write_atomic(&allocator, output_text, replacement,
                                      sizeof(replacement)));
  published = NULL;
  published_size = 0u;
  assert(file_open(&output, read_mode, &file) == FILE_ERROR_NONE);
  assert(file_read_all(&file, &allocator, &published, &published_size) ==
         FILE_ERROR_NONE);
  file_close(&file);
  assert(published_size == sizeof(replacement) &&
         MemCompare(published, replacement, sizeof(replacement)) == 0);

  /* Renaming a temporary regular file over the existing build directory must
     fail, leave that destination intact, and clean the temporary file. */
  FilePath build_directory =
      file_path_create("build", &allocator, FILE_PATH_TYPE_RELATIVE);
  assert(file_exists(&build_directory));
  assert(!vkr_font_cooked_write_atomic(&allocator, string8_lit("build"),
                                       artifact, artifact_size));
  assert(file_exists(&build_directory));

  assert(file_remove(&output) == FILE_ERROR_NONE);
  assert(file_remove(&collision) == FILE_ERROR_NONE);
  vkr_allocator_release_global_accounting(&allocator);
  arena_destroy(arena);
  printf("  test_font_cooked_atomic_publication PASSED\n");
}

typedef enum TestFontMutation {
  TEST_FONT_TRUNCATED,
  TEST_FONT_DECLARED_SIZE,
  TEST_FONT_OVERLAP,
  TEST_FONT_TRAILING,
  TEST_FONT_CHECKSUM,
  TEST_FONT_NAN,
  TEST_FONT_UNSORTED,
  TEST_FONT_DUPLICATE,
  TEST_FONT_BAD_REF,
  TEST_FONT_BAD_UV,
  TEST_FONT_VERSION,
  TEST_FONT_FIELD_KIND,
  TEST_FONT_MULTI_PAGE,
  TEST_FONT_PAGE_PIXEL_FORMAT,
  TEST_FONT_PAGE_ROW_STRIDE,
  TEST_FONT_PAGE_PIXEL_SIZE,
  TEST_FONT_NONCANONICAL_GAP,
  TEST_FONT_NONZERO_PADDING,
} TestFontMutation;

static bool8_t test_font_mutation_rejected(const uint8_t *source, uint64_t size,
                                           TestFontMutation mutation,
                                           VkrAllocator *result) {
  uint8_t *copy = malloc((size_t)size + 1u);
  assert(copy != NULL);
  MemCopy(copy, source, size);
  uint64_t test_size = size;
  switch (mutation) {
  case TEST_FONT_TRUNCATED:
    test_size -= 1u;
    break;
  case TEST_FONT_DECLARED_SIZE:
    test_font_write_u32(copy + 48u, 1u);
    break;
  case TEST_FONT_OVERLAP:
    test_font_write_u64(copy + VKR_FONT_COOKED_DIRECTORY_OFFSET + 32u + 8u,
                        test_font_read_u64(copy + 256u + 8u));
    break;
  case TEST_FONT_TRAILING:
    copy[size] = 0x5au;
    test_size = size + 1u;
    break;
  case TEST_FONT_CHECKSUM:
    copy[VKR_FONT_COOKED_DATA_OFFSET] ^= 1u;
    break;
  case TEST_FONT_NAN:
    test_font_write_u32(copy + test_font_read_u64(copy + 224u + 8u) + 12u,
                        0x7fc00000u);
    break;
  case TEST_FONT_UNSORTED:
    test_font_write_u32(copy + test_font_read_u64(copy + 224u + 8u) + 48u, 0u);
    break;
  case TEST_FONT_DUPLICATE:
    test_font_write_u32(copy + test_font_read_u64(copy + 256u + 8u) + 8u,
                        0x20u);
    break;
  case TEST_FONT_BAD_REF:
    test_font_write_u32(copy + test_font_read_u64(copy + 256u + 8u) + 12u, 99u);
    break;
  case TEST_FONT_BAD_UV:
    test_font_write_f32(copy + test_font_read_u64(copy + 224u + 8u) + 32u,
                        1.5f);
    break;
  case TEST_FONT_VERSION:
    test_font_write_u32(copy + 4u, 2u);
    break;
  case TEST_FONT_FIELD_KIND:
    test_font_write_u32(copy + 20u, 99u);
    break;
  case TEST_FONT_MULTI_PAGE:
    test_font_write_u32(copy + 44u, 2u);
    break;
  case TEST_FONT_PAGE_PIXEL_FORMAT:
    test_font_write_u32(copy + test_font_read_u64(copy + 320u + 8u) + 12u, 99u);
    break;
  case TEST_FONT_PAGE_ROW_STRIDE:
    test_font_write_u32(copy + test_font_read_u64(copy + 320u + 8u) + 8u, 4u);
    break;
  case TEST_FONT_PAGE_PIXEL_SIZE:
    test_font_write_u64(copy + test_font_read_u64(copy + 320u + 8u) + 24u, 12u);
    break;
  case TEST_FONT_NONCANONICAL_GAP:
    test_font_write_u32(copy + 56u, 16u);
    test_font_write_u64(copy + VKR_FONT_COOKED_DIRECTORY_OFFSET + 16u, 16u);
    copy[VKR_FONT_COOKED_DATA_OFFSET + 16u] = 0u;
    break;
  case TEST_FONT_NONZERO_PADDING:
    copy[VKR_FONT_COOKED_DATA_OFFSET + 17u] = 0x7fu;
    break;
  }
  if (mutation != TEST_FONT_TRUNCATED && mutation != TEST_FONT_TRAILING &&
      mutation != TEST_FONT_CHECKSUM && mutation != TEST_FONT_VERSION) {
    test_font_refresh_checksums(copy, test_size);
  }
  VkrFontCookedInspection inspection = {0};
  VkrFontCookedDecoded decoded = {0};
  const bool8_t inspected =
      vkr_font_cooked_inspect(copy, test_size, &inspection);
  const bool8_t rejected =
      !vkr_font_cooked_decode(result, copy, test_size, &decoded);
  assert(!inspected);
  assert(decoded.face.str == NULL && decoded.glyphs == NULL &&
         decoded.pages == NULL);
  free(copy);
  return rejected;
}

static void test_font_cooked_adversarial_boundaries(void) {
  printf("  Running test_font_cooked_adversarial_boundaries...\n");
  TestFontFixture fixture = test_font_fixture();
  Arena *scratch_arena = arena_create(MB(2), KB(64));
  Arena *result_arena = arena_create(MB(2), KB(64));
  assert(scratch_arena && result_arena);
  VkrAllocator scratch = {.ctx = scratch_arena};
  VkrAllocator result = {.ctx = result_arena};
  assert(vkr_allocator_arena(&scratch) && vkr_allocator_arena(&result));
  uint8_t *data = NULL;
  uint64_t size = 0u;
  assert(vkr_font_cooked_encode(&scratch, &fixture.info, &data, &size));
  for (TestFontMutation mutation = TEST_FONT_TRUNCATED;
       mutation <= TEST_FONT_NONZERO_PADDING;
       mutation = (TestFontMutation)(mutation + 1))
    assert(test_font_mutation_rejected(data, size, mutation, &result));
  VkrFontCookedInspection oversized_inspection = {0};
  VkrFontCookedDecoded oversized_decoded = {0};
  assert(!vkr_font_cooked_inspect(data, VKR_FONT_COOKED_MAX_FILE_SIZE + 1u,
                                  &oversized_inspection));
  assert(!vkr_font_cooked_decode(
      &result, data, VKR_FONT_COOKED_MAX_FILE_SIZE + 1u, &oversized_decoded));
  vkr_allocator_release_global_accounting(&scratch);
  vkr_allocator_release_global_accounting(&result);
  arena_destroy(scratch_arena);
  arena_destroy(result_arena);
  printf("  test_font_cooked_adversarial_boundaries PASSED\n");
}

static const VkrFontCookedCodepoint *
test_font_find_codepoint(const VkrFontCookedDecoded *font, uint32_t codepoint) {
  uint32_t lo = 0u;
  uint32_t hi = font->codepoint_count;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2u;
    const uint32_t candidate = font->codepoints[mid].codepoint;
    if (candidate < codepoint)
      lo = mid + 1u;
    else
      hi = mid;
  }
  return lo < font->codepoint_count &&
                 font->codepoints[lo].codepoint == codepoint
             ? &font->codepoints[lo]
             : NULL;
}

static void test_font_cooked_production_coverage(void) {
  printf("  Running test_font_cooked_production_coverage...\n");
  Arena *arena = arena_create(MB(8), MB(8));
  assert(arena);
  VkrAllocator allocator = {.ctx = arena};
  assert(vkr_allocator_arena(&allocator));
  VkrAllocatorScope decode_scope = vkr_allocator_begin_scope(&allocator);
  assert(vkr_allocator_scope_is_valid(&decode_scope));
  FilePath path = file_path_create("assets/fonts/UbuntuMono-cooked.vkfa",
                                   &allocator, FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);
  FileHandle file = {0};
  assert(file_open(&path, mode, &file) == FILE_ERROR_NONE);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  assert(file_read_all(&file, &allocator, &bytes, &size) == FILE_ERROR_NONE);
  file_close(&file);

  VkrFontCookedDecoded decoded = {0};
  assert(vkr_font_cooked_decode(&allocator, bytes, size, &decoded));
  assert(decoded.codepoint_count == 191u);
  for (uint32_t codepoint = 0x20u; codepoint <= 0x7eu; ++codepoint)
    assert(test_font_find_codepoint(&decoded, codepoint));
  for (uint32_t codepoint = 0xa0u; codepoint <= 0xffu; ++codepoint)
    assert(test_font_find_codepoint(&decoded, codepoint));
  const VkrFontCookedCodepoint *fallback =
      test_font_find_codepoint(&decoded, '?');
  assert(fallback && fallback->glyph_id == decoded.fallback_glyph_id);

#if !VKR_ALLOCATOR_DISABLE_STATS
  assert(allocator.stats.peak_temp_bytes > size);
  assert(allocator.stats.peak_temp_bytes < size + KB(64));
  printf("    production decode peak temporary bytes: %llu\n",
         (unsigned long long)allocator.stats.peak_temp_bytes);
#endif
  vkr_allocator_end_scope(&decode_scope, VKR_ALLOCATOR_MEMORY_TAG_FILE);
  vkr_allocator_release_global_accounting(&allocator);
  arena_destroy(arena);
  printf("  test_font_cooked_production_coverage PASSED\n");
}

bool32_t run_font_cooked_tests(void) {
  test_font_cooked_round_trip_and_golden();
  test_font_cooked_atomic_publication();
  test_font_cooked_adversarial_boundaries();
  test_font_cooked_production_coverage();
  return true;
}
