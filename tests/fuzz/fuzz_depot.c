/* Depot fuzzer: object parsers, stored representations, protocol messages
 * and discovery datagrams (ADR-105, "Security").
 *
 * The input's first byte picks a target; the rest is the target's bytes:
 *   0  blob manifest      parse; an accepted manifest re-encodes to itself
 *   1  tree               parse; an accepted tree re-encodes to itself
 *   2  commit             parse; an accepted commit re-encodes to itself
 *   3  stored form        decode raw and zstd representations
 *   4  protocol message   decode with the depot schema; re-encodes to itself
 *   5  discovery probe    read; re-written to itself
 *   6  discovery answer   read; re-written to itself where text was printable
 *   7  names and IDs      name validation and hexadecimal ID parsing
 *
 * Every parser reads an exact-size heap copy, so a read past the input
 * reaches ASan. A parser that accepts bytes its own encoder would not
 * write breaks the formats' one-canonical-form rule. */

#include "fuzz_common.h"
#include "vkr_depot_discovery.h"
#include "vkr_depot_object.h"
#include "vkr_depot_protocol.h"
#include "vkr_depot_store.h"
#include "vkr_depot_zstd.h"

#define FUZZ_DEPOT_TREE_ENTRY_MIN (2u + 1u + 8u + VKR_DEPOT_ID_SIZE)

enum {
  FUZZ_DEPOT_BLOB = 0,
  FUZZ_DEPOT_TREE,
  FUZZ_DEPOT_COMMIT,
  FUZZ_DEPOT_STORED,
  FUZZ_DEPOT_MESSAGE,
  FUZZ_DEPOT_PROBE,
  FUZZ_DEPOT_ANSWER,
  FUZZ_DEPOT_NAMES,
  FUZZ_DEPOT_TARGETS,
};

static void fuzz_depot_check_equal(const uint8_t *encoded,
                                   uint64_t encoded_size, const uint8_t *input,
                                   uint64_t size) {
  FUZZ_CHECK(encoded_size == size);
  FUZZ_CHECK(MemCompare(encoded, input, (size_t)size) == 0);
}

// =============================================================================
// Objects
// =============================================================================

typedef struct FuzzDepotChunks {
  VkrDepotChunkRef *chunks;
  uint32_t count;
  uint32_t capacity;
} FuzzDepotChunks;

static bool8_t fuzz_depot_chunk_visit(void *context,
                                      const VkrDepotChunkRef *chunk) {
  FuzzDepotChunks *list = context;
  FUZZ_CHECK(list->count < list->capacity);
  FUZZ_CHECK(chunk->size > 0u);
  list->chunks[list->count++] = *chunk;
  return true_v;
}

static void fuzz_depot_blob(const uint8_t *bytes, uint32_t size) {
  uint64_t file_size = 0u;
  if (!vkr_depot_blob_parse(bytes, size, &file_size, NULL, NULL)) {
    return;
  }
  /* The header's count is bounded by the bytes that hold the chunks. */
  FuzzDepotChunks list = {.capacity = size / (VKR_DEPOT_ID_SIZE + 4u) + 1u};
  list.chunks = malloc(sizeof(VkrDepotChunkRef) * list.capacity);
  FUZZ_CHECK(list.chunks);
  FUZZ_CHECK(
      vkr_depot_blob_parse(bytes, size, NULL, fuzz_depot_chunk_visit, &list));
  uint64_t total = 0u;
  for (uint32_t i = 0u; i < list.count; ++i) {
    total += list.chunks[i].size;
  }
  FUZZ_CHECK(total == file_size);
  uint8_t *encoded = NULL;
  uint64_t encoded_size = 0u;
  FUZZ_CHECK(
      vkr_depot_blob_encode(list.chunks, list.count, &encoded, &encoded_size));
  fuzz_depot_check_equal(encoded, encoded_size, bytes, size);
  free(encoded);
  free(list.chunks);
}

typedef struct FuzzDepotEntries {
  VkrDepotTreeEntry *entries;
  uint32_t count;
  uint32_t capacity;
  const uint8_t *bytes;
  uint32_t size;
} FuzzDepotEntries;

static bool8_t fuzz_depot_entry_visit(void *context,
                                      const VkrDepotTreeEntry *entry) {
  FuzzDepotEntries *list = context;
  FUZZ_CHECK(list->count < list->capacity);
  /* Names are borrowed from the parsed bytes. */
  FUZZ_CHECK((const uint8_t *)entry->name >= list->bytes &&
             (const uint8_t *)entry->name + entry->name_size <=
                 list->bytes + list->size);
  FUZZ_CHECK(vkr_depot_name_valid(entry->name, entry->name_size));
  FUZZ_CHECK(entry->kind == VKR_DEPOT_ENTRY_FILE ||
             entry->kind == VKR_DEPOT_ENTRY_TREE);
  list->entries[list->count++] = *entry;
  return true_v;
}

static void fuzz_depot_tree(const uint8_t *bytes, uint32_t size) {
  FuzzDepotEntries list = {
      .capacity = size / FUZZ_DEPOT_TREE_ENTRY_MIN + 1u,
      .bytes = bytes,
      .size = size,
  };
  list.entries = malloc(sizeof(VkrDepotTreeEntry) * list.capacity);
  FUZZ_CHECK(list.entries);
  if (!vkr_depot_tree_parse(bytes, size, fuzz_depot_entry_visit, &list)) {
    free(list.entries);
    return;
  }
  uint8_t *encoded = NULL;
  uint64_t encoded_size = 0u;
  /* The encoder refuses names equal under case folding; a parser that
     accepts them lets one tree hold two files a checkout writes to one
     path on macOS and Windows. */
  FUZZ_CHECK(
      vkr_depot_tree_encode(list.entries, list.count, &encoded, &encoded_size));
  fuzz_depot_check_equal(encoded, encoded_size, bytes, size);
  free(encoded);
  free(list.entries);
}

static void fuzz_depot_commit(const uint8_t *bytes, uint32_t size) {
  VkrDepotCommit commit;
  if (!vkr_depot_commit_parse(bytes, size, &commit)) {
    return;
  }
  FUZZ_CHECK(commit.parent_count <= VKR_DEPOT_PARENTS_MAX);
  FUZZ_CHECK(commit.message_size <= VKR_DEPOT_MESSAGE_MAX);
  FUZZ_CHECK(
      commit.message_size == 0u ||
      ((const uint8_t *)commit.message >= bytes &&
       (const uint8_t *)commit.message + commit.message_size == bytes + size));
  uint8_t *encoded = NULL;
  uint64_t encoded_size = 0u;
  FUZZ_CHECK(vkr_depot_commit_encode(&commit, &encoded, &encoded_size));
  fuzz_depot_check_equal(encoded, encoded_size, bytes, size);
  free(encoded);
}

static void fuzz_depot_stored(const uint8_t *bytes, uint32_t size) {
  const VkrCodec *codecs[2] = {NULL, vkr_depot_zstd_codec()};
  for (uint32_t i = 0u; i < ArrayCount(codecs); ++i) {
    uint8_t *decoded = NULL;
    uint64_t decoded_size = 0u;
    if (!vkr_depot_stored_decode(codecs[i], bytes, size, &decoded,
                                 &decoded_size)) {
      continue;
    }
    FUZZ_CHECK(decoded);
    FUZZ_CHECK(decoded_size <= VKR_DEPOT_OBJECT_MAX);
    FUZZ_CHECK(size >= VKR_DEPOT_STORED_HEADER);
    /* The header names the decoded size. */
    FUZZ_CHECK(decoded_size ==
               (uint64_t)(bytes[1] | (bytes[2] << 8) | (bytes[3] << 16) |
                          ((uint32_t)bytes[4] << 24)));
    uint64_t sum = 0u;
    for (uint64_t k = 0u; k < decoded_size; ++k) {
      sum += decoded[k];
    }
    (void)sum;
    free(decoded);
  }
}

// =============================================================================
// Protocol and discovery
// =============================================================================

static void fuzz_depot_message(const uint8_t *bytes, uint32_t size) {
  const VkrWireSchema *schema = vkr_depot_schema();
  const uint32_t id = vkr_wire_peek_id(bytes, size);
  const VkrWireMessage *expected =
      id <= 0xffffu ? vkr_depot_message((VkrDepotMessageId)id) : NULL;
  /* Exactly the message's struct, so a write past it reaches ASan. */
  const uint32_t capacity =
      expected ? expected->size : vkr_wire_schema_max_size(schema);
  uint8_t *value = malloc(capacity);
  FUZZ_CHECK(value);
  const VkrWireMessage *message = NULL;
  if (vkr_wire_decode(schema, bytes, size, value, capacity, &message) !=
      VKR_WIRE_OK) {
    free(value);
    return;
  }
  FUZZ_CHECK(message == expected);
  static uint8_t encoded[VKR_DEPOT_BATCH * VKR_DEPOT_ID_SIZE * 2u];
  uint32_t encoded_size = 0u;
  FUZZ_CHECK(vkr_wire_encode(message, value, encoded, sizeof(encoded),
                             &encoded_size) == VKR_WIRE_OK);
  fuzz_depot_check_equal(encoded, encoded_size, bytes, size);

  /* The checks a server applies to decoded names. */
  if (message->id == VKR_DEPOT_MSG_UPDATE_REF) {
    (void)vkr_depot_branch_valid(((const VkrDepotUpdateRefWire *)value)->name);
  } else if (message->id == VKR_DEPOT_MSG_REFS) {
    const VkrDepotRefsWire *refs = (const VkrDepotRefsWire *)value;
    FUZZ_CHECK(refs->ref_count <= VKR_DEPOT_REFS_MAX);
    for (uint32_t i = 0u; i < refs->ref_count; ++i) {
      (void)vkr_depot_branch_valid(refs->refs[i].name);
    }
  }
  free(value);
}

static void fuzz_depot_probe(const uint8_t *bytes, uint32_t size) {
  uint64_t nonce = 0u;
  if (!vkr_depot_discovery_read_probe(bytes, size, &nonce)) {
    return;
  }
  uint8_t written[192];
  const uint32_t written_size = vkr_depot_discovery_probe(nonce, written);
  fuzz_depot_check_equal(written, written_size, bytes, size);
}

static void fuzz_depot_answer(const uint8_t *bytes, uint32_t size) {
  if (size < 14u) {
    return;
  }
  /* Answer the nonce the bytes carry, as a client that sent it would. */
  uint64_t nonce = 0u;
  for (uint32_t i = 0u; i < 8u; ++i) {
    nonce |= (uint64_t)bytes[6u + i] << (8u * i);
  }
  VkrDepotDiscovered found;
  if (!vkr_depot_discovery_read_answer(bytes, size, nonce, &found)) {
    return;
  }
  FUZZ_CHECK(found.address.port != 0u);
  FUZZ_CHECK(memchr(found.name, 0, sizeof(found.name)));
  FUZZ_CHECK(memchr(found.host, 0, sizeof(found.host)));
  VkrDepotResponder responder = {.service_port = found.address.port};
  MemCopy(responder.server_key, found.server_key, 32u);
  MemCopy(responder.name, found.name, sizeof(found.name));
  MemCopy(responder.host, found.host, sizeof(found.host));
  uint8_t written[192];
  const uint32_t written_size =
      vkr_depot_discovery_answer(&responder, nonce, written);
  /* Unprintable text bytes come back as '?'; everything else matches. */
  FUZZ_CHECK(written_size == size);
  for (uint32_t i = 0u; i < size; ++i) {
    const bool8_t printable = bytes[i] >= 0x20u && bytes[i] < 0x7fu;
    FUZZ_CHECK(written[i] == bytes[i] || (written[i] == '?' && !printable));
  }
}

static void fuzz_depot_names(const uint8_t *bytes, uint32_t size) {
  (void)vkr_depot_name_valid((const char *)bytes, size);
  (void)vkr_depot_object_kind(bytes, size);
  char hex[VKR_DEPOT_ID_HEX + 1u];
  const uint32_t length = Min(size, (uint32_t)VKR_DEPOT_ID_HEX);
  MemCopy(hex, bytes, length);
  hex[length] = '\0';
  VkrDepotId id;
  if (vkr_depot_id_parse(hex, &id)) {
    char again[VKR_DEPOT_ID_HEX];
    vkr_depot_id_hex(&id, again);
    FUZZ_CHECK(strcmp(again, hex) == 0);
  }
  char branch[VKR_DEPOT_BRANCH_NAME + 8u];
  const uint32_t branch_length = Min(size, (uint32_t)sizeof(branch) - 1u);
  MemCopy(branch, bytes, branch_length);
  branch[branch_length] = '\0';
  (void)vkr_depot_branch_valid(branch);
}

// =============================================================================
// Entry
// =============================================================================

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 1u || size > 1048576u) {
    return 0;
  }
  const uint32_t rest = (uint32_t)size - 1u;
  uint8_t *bytes = malloc(rest ? rest : 1u);
  FUZZ_CHECK(bytes);
  if (rest > 0u) {
    MemCopy(bytes, data + 1, rest);
  }
  switch (data[0] % FUZZ_DEPOT_TARGETS) {
  case FUZZ_DEPOT_BLOB:
    fuzz_depot_blob(bytes, rest);
    break;
  case FUZZ_DEPOT_TREE:
    fuzz_depot_tree(bytes, rest);
    break;
  case FUZZ_DEPOT_COMMIT:
    fuzz_depot_commit(bytes, rest);
    break;
  case FUZZ_DEPOT_STORED:
    fuzz_depot_stored(bytes, rest);
    break;
  case FUZZ_DEPOT_MESSAGE:
    fuzz_depot_message(bytes, rest);
    break;
  case FUZZ_DEPOT_PROBE:
    fuzz_depot_probe(bytes, rest);
    break;
  case FUZZ_DEPOT_ANSWER:
    fuzz_depot_answer(bytes, rest);
    break;
  default:
    fuzz_depot_names(bytes, rest);
    break;
  }
  free(bytes);
  return 0;
}

// =============================================================================
// Seeds
// =============================================================================

static void fuzz_depot_seed(FuzzSeedWriter *writer, uint8_t target,
                            const uint8_t *bytes, uint64_t size) {
  FuzzSeed seed = {0};
  fuzz_seed_u8(&seed, target);
  fuzz_seed_bytes(&seed, bytes, (uint32_t)size);
  fuzz_seed_emit(writer, &seed);
}

static void fuzz_depot_seed_id(VkrDepotId *id, uint8_t label) {
  for (uint32_t i = 0u; i < VKR_DEPOT_ID_SIZE; ++i) {
    id->bytes[i] = (uint8_t)(label + i);
  }
}

static void fuzz_depot_seed_objects(FuzzSeedWriter *writer) {
  uint8_t *bytes = NULL;
  uint64_t size = 0u;

  VkrDepotChunkRef chunks[3];
  for (uint32_t i = 0u; i < ArrayCount(chunks); ++i) {
    fuzz_depot_seed_id(&chunks[i].id, (uint8_t)(i * 40u));
    chunks[i].size = 65536u + i;
  }
  FUZZ_CHECK(vkr_depot_blob_encode(chunks, ArrayCount(chunks), &bytes, &size));
  fuzz_depot_seed(writer, FUZZ_DEPOT_BLOB, bytes, size);
  free(bytes);
  FUZZ_CHECK(vkr_depot_blob_encode(chunks, 0u, &bytes, &size));
  fuzz_depot_seed(writer, FUZZ_DEPOT_BLOB, bytes, size);
  free(bytes);

  VkrDepotTreeEntry entries[4] = {
      {.name = "textures", .name_size = 8u, .kind = VKR_DEPOT_ENTRY_TREE},
      {.name = "Bistro.vkpak",
       .name_size = 12u,
       .kind = VKR_DEPOT_ENTRY_FILE,
       .size = 123456u},
      {.name = "na\xc3\xafve.txt",
       .name_size = 10u,
       .kind = VKR_DEPOT_ENTRY_FILE,
       .size = 1u},
      {.name = "a b", .name_size = 3u, .kind = VKR_DEPOT_ENTRY_FILE},
  };
  for (uint32_t i = 0u; i < ArrayCount(entries); ++i) {
    fuzz_depot_seed_id(&entries[i].id, (uint8_t)(i * 9u));
  }
  FUZZ_CHECK(
      vkr_depot_tree_encode(entries, ArrayCount(entries), &bytes, &size));
  fuzz_depot_seed(writer, FUZZ_DEPOT_TREE, bytes, size);

  /* The tree's stored forms, raw and zstd, for the stored-form target. */
  const VkrCodec *codec = vkr_depot_zstd_codec();
  uint8_t stored[4096];
  FUZZ_CHECK(size + VKR_DEPOT_STORED_HEADER <= sizeof(stored));
  stored[0] = 0u;
  stored[1] = (uint8_t)size;
  stored[2] = (uint8_t)(size >> 8);
  stored[3] = 0u;
  stored[4] = 0u;
  MemCopy(stored + VKR_DEPOT_STORED_HEADER, bytes, size);
  fuzz_depot_seed(writer, FUZZ_DEPOT_STORED, stored,
                  size + VKR_DEPOT_STORED_HEADER);
  stored[0] = 1u;
  const uint64_t compressed =
      codec->compress(stored + VKR_DEPOT_STORED_HEADER,
                      sizeof(stored) - VKR_DEPOT_STORED_HEADER, bytes, size, 3);
  FUZZ_CHECK(compressed > 0u);
  fuzz_depot_seed(writer, FUZZ_DEPOT_STORED, stored,
                  compressed + VKR_DEPOT_STORED_HEADER);
  free(bytes);

  VkrDepotCommit commit = {.parent_count = 2u,
                           .time_us = 1760000000000000ull,
                           .message = "Bistro lighting pass",
                           .message_size = 20u};
  fuzz_depot_seed_id(&commit.tree, 1u);
  fuzz_depot_seed_id(&commit.parents[0], 2u);
  fuzz_depot_seed_id(&commit.parents[1], 3u);
  MemSet(commit.author, 0x42, sizeof(commit.author));
  FUZZ_CHECK(vkr_depot_commit_encode(&commit, &bytes, &size));
  fuzz_depot_seed(writer, FUZZ_DEPOT_COMMIT, bytes, size);
  free(bytes);
}

static void fuzz_depot_seed_message(FuzzSeedWriter *writer,
                                    VkrDepotMessageId id, const void *value) {
  static uint8_t encoded[VKR_DEPOT_BATCH * VKR_DEPOT_ID_SIZE * 2u];
  uint32_t size = 0u;
  FUZZ_CHECK(vkr_wire_encode(vkr_depot_message(id), value, encoded,
                             sizeof(encoded), &size) == VKR_WIRE_OK);
  fuzz_depot_seed(writer, FUZZ_DEPOT_MESSAGE, encoded, size);
}

static void fuzz_depot_seed_messages(FuzzSeedWriter *writer) {
  static VkrDepotMessage message;
  MemZero(&message, sizeof(message));
  message.request.request = 7u;
  fuzz_depot_seed_message(writer, VKR_DEPOT_MSG_REFS_REQUEST, &message);

  MemZero(&message, sizeof(message));
  message.refs.request = 8u;
  message.refs.ref_count = 2u;
  snprintf(message.refs.refs[0].name, VKR_DEPOT_BRANCH_NAME, "main");
  snprintf(message.refs.refs[1].name, VKR_DEPOT_BRANCH_NAME, "art-pass.2");
  message.refs.refs[1].commit[0] = 0xab;
  fuzz_depot_seed_message(writer, VKR_DEPOT_MSG_REFS, &message);

  MemZero(&message, sizeof(message));
  message.ids.request = 300u;
  message.ids.zstd = true_v;
  message.ids.id_count = 3u;
  for (uint32_t i = 0u; i < 3u; ++i) {
    MemSet(message.ids.ids[i], (int)(i + 1u), VKR_DEPOT_ID_SIZE);
  }
  fuzz_depot_seed_message(writer, VKR_DEPOT_MSG_GET, &message);

  MemZero(&message, sizeof(message));
  message.bits.request = 9u;
  message.bits.bits_size = 2u;
  message.bits.bits[0] = 0x81u;
  fuzz_depot_seed_message(writer, VKR_DEPOT_MSG_HAVE_REPLY, &message);

  MemZero(&message, sizeof(message));
  message.update_ref.request = 10u;
  snprintf(message.update_ref.name, VKR_DEPOT_BRANCH_NAME, "main");
  message.update_ref.commit[31] = 1u;
  fuzz_depot_seed_message(writer, VKR_DEPOT_MSG_UPDATE_REF, &message);

  MemZero(&message, sizeof(message));
  message.update_ref_reply.request = 10u;
  message.update_ref_reply.status = VKR_DEPOT_REF_CONFLICT;
  fuzz_depot_seed_message(writer, VKR_DEPOT_MSG_UPDATE_REF_REPLY, &message);

  MemZero(&message, sizeof(message));
  message.lock.request = 11u;
  snprintf(message.lock.path, VKR_DEPOT_LOCK_PATH_MAX,
           "assets/scenes/bistro/bistro.vkscene");
  message.lock.release = true_v;
  fuzz_depot_seed_message(writer, VKR_DEPOT_MSG_LOCK, &message);

  MemZero(&message, sizeof(message));
  message.lock_reply.status = VKR_DEPOT_LOCK_HELD;
  message.lock_reply.owner[0] = 9u;
  fuzz_depot_seed_message(writer, VKR_DEPOT_MSG_LOCK_REPLY, &message);

  MemZero(&message, sizeof(message));
  snprintf(message.locks_request.after, VKR_DEPOT_LOCK_PATH_MAX, "a/b");
  fuzz_depot_seed_message(writer, VKR_DEPOT_MSG_LOCKS_REQUEST, &message);

  MemZero(&message, sizeof(message));
  message.locks.entry_count = 2u;
  message.locks.more = true_v;
  snprintf(message.locks.entries[0].path, VKR_DEPOT_LOCK_PATH_MAX, "x.mt");
  snprintf(message.locks.entries[1].path, VKR_DEPOT_LOCK_PATH_MAX, "y.mt");
  message.locks.entries[1].time_us = 5u;
  fuzz_depot_seed_message(writer, VKR_DEPOT_MSG_LOCKS, &message);
}

static void fuzz_depot_seed_discovery(FuzzSeedWriter *writer) {
  uint8_t bytes[192];
  uint32_t size = vkr_depot_discovery_probe(0x1122334455667788ull, bytes);
  fuzz_depot_seed(writer, FUZZ_DEPOT_PROBE, bytes, size);

  VkrDepotResponder responder = {.service_port = 7320u};
  MemSet(responder.server_key, 0x33, sizeof(responder.server_key));
  snprintf(responder.name, sizeof(responder.name), "studio depot");
  snprintf(responder.host, sizeof(responder.host), "mac-m1.local");
  size = vkr_depot_discovery_answer(&responder, 0x1122334455667788ull, bytes);
  fuzz_depot_seed(writer, FUZZ_DEPOT_ANSWER, bytes, size);

  static const char id_hex[] =
      "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
  fuzz_depot_seed(writer, FUZZ_DEPOT_NAMES, (const uint8_t *)id_hex,
                  sizeof(id_hex) - 1u);
}

static void fuzz_depot_seeds(FuzzSeedWriter *writer) {
  fuzz_depot_seed_objects(writer);
  fuzz_depot_seed_messages(writer);
  fuzz_depot_seed_discovery(writer);
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
  (void)argc;
  (void)argv;
  FUZZ_CHECK(vkr_wire_schema_validate(vkr_depot_schema()));
  fuzz_seed_mode(fuzz_depot_seeds);
  return 0;
}
