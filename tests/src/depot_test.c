#include "depot_test.h"
#include "core/vkr_threads.h"
#include "filesystem/filesystem.h"
#include "filesystem/vkr_vfs.h"

#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "platform/vkr_platform.h"
#include "vkr_bakery_os.h"
#include "vkr_depot_client.h"
#include "vkr_depot_remote.h"
#include "vkr_depot_server.h"
#include "vkr_depot_service.h"
#include "vkr_depot_sync.h"
#include "vkr_depot_workspace.h"
#include "vkr_depot_zstd.h"
#include "vkr_net_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#endif

/* The asset depot (docs/proposals/network-protocol.md, "Depot"): object
 * formats, the store, content-defined chunking, working copies and the
 * client and server over the simulated network. Each test names the failure
 * it detects. Directories live under tests/tmp and are removed. */

static uint32_t depot_test_serial = 0u;

static void depot_test_dir(const char *label, char *out, uint32_t capacity) {
  snprintf(out, capacity, "%stests/tmp/depot_%u_%u_%s", PROJECT_SOURCE_DIR,
           vkr_platform_get_process_id(), ++depot_test_serial, label);
  assert(vkr_bakery_remove_tree(out));
  assert(vkr_bakery_make_directories(out));
}

static void depot_test_write(const char *root, const char *relative,
                             const void *data, uint64_t size) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  char directory[VKR_DEPOT_PATH_CAPACITY];
  assert(vkr_bakery_path_join(path, sizeof(path), root, relative));
  vkr_bakery_path_parent(directory, sizeof(directory), path);
  assert(vkr_bakery_make_directories(directory));
  assert(vkr_bakery_write_file_atomic(path, data, size));
}

static uint64_t depot_test_state = 99u;

static void depot_test_random(uint8_t *out, uint64_t size) {
  for (uint64_t i = 0u; i < size; ++i) {
    depot_test_state ^= depot_test_state << 13;
    depot_test_state ^= depot_test_state >> 7;
    depot_test_state ^= depot_test_state << 17;
    out[i] = (uint8_t)depot_test_state;
  }
}

static bool8_t depot_test_files_equal(const char *a, const char *b) {
  uint8_t *x = NULL;
  uint8_t *y = NULL;
  uint64_t xs = 0u;
  uint64_t ys = 0u;
  const bool8_t ok = vkr_bakery_read_file(a, 1ull << 30, &x, &xs) &&
                     vkr_bakery_read_file(b, 1ull << 30, &y, &ys) && xs == ys &&
                     memcmp(x, y, xs) == 0;
  free(x);
  free(y);
  return ok;
}

// =============================================================================
// Objects, store and chunking
// =============================================================================

typedef struct EntryCollector {
  char names[8][32];
  uint32_t count;
} EntryCollector;

static bool8_t depot_test_collect(void *context,
                                  const VkrDepotTreeEntry *entry) {
  EntryCollector *collector = context;
  snprintf(collector->names[collector->count++], 32u, "%.*s",
           (int)entry->name_size, entry->name);
  return true_v;
}

/* Fails when trees, blobs or commits do not round-trip, or when a tree with
   case-colliding or invalid names (encoded or parsed), or an object with
   trailing or missing bytes or inconsistent sizes, is accepted. */
static void test_depot_objects(void) {
  printf("  Running test_depot_objects...\n");
  VkrDepotTreeEntry entries[3] = {
      {.name = "zeta.png",
       .name_size = 8,
       .kind = VKR_DEPOT_ENTRY_FILE,
       .size = 10},
      {.name = "alpha", .name_size = 5, .kind = VKR_DEPOT_ENTRY_TREE},
      {.name = "\xd0\xb4\xd0\xbe\xd0\xbc",
       .name_size = 6,
       .kind = VKR_DEPOT_ENTRY_FILE,
       .size = 3},
  };
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  assert(vkr_depot_tree_encode(entries, 3u, &bytes, &size));
  EntryCollector collector = {0};
  assert(vkr_depot_tree_parse(bytes, size, depot_test_collect, &collector));
  assert(collector.count == 3u && strcmp(collector.names[0], "alpha") == 0 &&
         strcmp(collector.names[1], "zeta.png") == 0);
  /* A trailing byte is not canonical. */
  uint8_t *longer = malloc(size + 1u);
  memcpy(longer, bytes, size);
  longer[size] = 0u;
  assert(!vkr_depot_tree_parse(longer, size + 1u, NULL, NULL));
  free(longer);
  free(bytes);

  VkrDepotTreeEntry collide[2] = {
      {.name = "Texture.png", .name_size = 11, .kind = VKR_DEPOT_ENTRY_FILE},
      {.name = "texture.png", .name_size = 11, .kind = VKR_DEPOT_ENTRY_FILE},
  };
  assert(!vkr_depot_tree_encode(collide, 2u, &bytes, &size));
  /* Names a checkout must never write: the depot's and git's state in any
     case, names Windows cannot hold or turns into devices, malformed or
     overlong UTF-8, surrogates, and decomposed letters that alias their
     composed form on macOS. */
  const char *invalid[] = {".",
                           "..",
                           "a/b",
                           "a\\b",
                           "trail.",
                           "trail ",
                           "\xff",
                           "\xc0\xaf",
                           "tab\there",
                           ".vkrdepot",
                           ".VkrDepot",
                           ".git",
                           ".GIT",
                           "a:b",
                           "a<b",
                           "a>b",
                           "a\"b",
                           "a|b",
                           "a?b",
                           "a*b",
                           "CON",
                           "con.txt",
                           "Com1.log",
                           "lpt9",
                           "nul .txt",
                           "\xe0\x80\xaf",
                           "\xf0\x80\x80\xaf",
                           "\xed\xa0\x80",
                           "\xf4\x90\x80\x80",
                           "e\xcc\x81"};
  for (uint32_t i = 0u; i < ArrayCount(invalid); ++i) {
    assert(!vkr_depot_name_valid(invalid[i], (uint32_t)strlen(invalid[i])));
  }
  assert(!vkr_depot_name_valid("", 0u));
  const char *valid[] = {"Bistro_Exterior.vkb", "com10.txt",
                         "console.log",         ".vkrdepotignore",
                         "\xc3\xa9t\xc3\xa9",   "\xd0\x9f\xd1\x80\xd0\xb8"};
  for (uint32_t i = 0u; i < ArrayCount(valid); ++i) {
    assert(vkr_depot_name_valid(valid[i], (uint32_t)strlen(valid[i])));
  }
  assert(vkr_depot_path_valid("a/b/c.txt"));
  assert(!vkr_depot_path_valid("a/../b"));
  assert(!vkr_depot_path_valid("/abs"));
  assert(!vkr_depot_path_valid("a/.vkrdepot/state"));
  /* Letters macOS folds beyond ASCII: \u00c4/\u00e4 and Cyrillic
     \u041f/\u043f. */
  VkrDepotTreeEntry folded_latin[2] = {
      {.name = "\xc3\x84.png", .name_size = 6, .kind = VKR_DEPOT_ENTRY_FILE},
      {.name = "\xc3\xa4.png", .name_size = 6, .kind = VKR_DEPOT_ENTRY_FILE},
  };
  assert(!vkr_depot_tree_encode(folded_latin, 2u, &bytes, &size));
  VkrDepotTreeEntry folded_cyrillic[2] = {
      {.name = "\xd0\x9f", .name_size = 2, .kind = VKR_DEPOT_ENTRY_FILE},
      {.name = "\xd0\xbf", .name_size = 2, .kind = VKR_DEPOT_ENTRY_FILE},
  };
  assert(!vkr_depot_tree_encode(folded_cyrillic, 2u, &bytes, &size));

  VkrDepotChunkRef chunks[2] = {{.size = 5u}, {.size = 7u}};
  chunks[0].id.bytes[0] = 1u;
  chunks[1].id.bytes[0] = 2u;
  assert(vkr_depot_blob_encode(chunks, 2u, &bytes, &size));
  uint64_t file_size = 0u;
  assert(vkr_depot_blob_parse(bytes, size, &file_size, NULL, NULL) &&
         file_size == 12u);
  /* A file size the chunks contradict. */
  bytes[VKR_DEPOT_HEADER_SIZE] = 13u;
  assert(!vkr_depot_blob_parse(bytes, size, NULL, NULL, NULL));
  free(bytes);

  /* One 1 MiB chunk repeated past VKR_DEPOT_FILE_MAX: a manifest a few
     hundred KiB long that would write a huge file. */
  const uint32_t repeats =
      (uint32_t)(VKR_DEPOT_FILE_MAX / VKR_DEPOT_CHUNK_MAX) + 1u;
  VkrDepotChunkRef *repeated = calloc(repeats, sizeof(VkrDepotChunkRef));
  for (uint32_t i = 0u; i < repeats; ++i) {
    repeated[i].size = VKR_DEPOT_CHUNK_MAX;
  }
  assert(vkr_depot_blob_encode(repeated, repeats, &bytes, &size));
  assert(!vkr_depot_blob_parse(bytes, size, NULL, NULL, NULL));
  free(bytes);
  free(repeated);

  VkrDepotCommit commit = {.parent_count = 1u,
                           .time_us = 42u,
                           .message = "first",
                           .message_size = 5u};
  commit.tree.bytes[0] = 9u;
  commit.parents[0].bytes[0] = 8u;
  assert(vkr_depot_commit_encode(&commit, &bytes, &size));
  VkrDepotCommit parsed;
  assert(vkr_depot_commit_parse(bytes, size, &parsed));
  assert(parsed.parent_count == 1u && parsed.tree.bytes[0] == 9u &&
         parsed.parents[0].bytes[0] == 8u && parsed.time_us == 42u &&
         parsed.message_size == 5u && memcmp(parsed.message, "first", 5u) == 0);
  assert(!vkr_depot_commit_parse(bytes, size - 1u, &parsed));
  free(bytes);

  /* A tree whose names differ only in case, sorted by bytes so `A` and `a`
     are not neighbours: a checkout would write two paths that are one file
     on macOS and Windows. */
  VkrDepotTreeEntry folded[3] = {
      {.name = "A", .name_size = 1, .kind = VKR_DEPOT_ENTRY_FILE},
      {.name = "B", .name_size = 1, .kind = VKR_DEPOT_ENTRY_FILE},
      {.name = "c", .name_size = 1, .kind = VKR_DEPOT_ENTRY_FILE},
  };
  assert(vkr_depot_tree_encode(folded, 3u, &bytes, &size));
  assert(vkr_depot_tree_parse(bytes, size, NULL, NULL));
  uint8_t *third = memchr(bytes + VKR_DEPOT_HEADER_SIZE + 4u, 'c',
                          size - VKR_DEPOT_HEADER_SIZE - 4u);
  assert(third);
  *third = 'a';
  assert(!vkr_depot_tree_parse(bytes, size, NULL, NULL));
  free(bytes);
  printf("  test_depot_objects PASSED\n");
}

/* Fails when the store accepts bytes under the wrong ID or a corrupted
   received representation, does not compress compressible data, keeps
   incompressible data compressed, or returns other bytes than it stored. */
static void test_depot_store(void) {
  printf("  Running test_depot_store...\n");
  char root[VKR_DEPOT_PATH_CAPACITY];
  depot_test_dir("store", root, sizeof(root));
  VkrDepotStore store;
  assert(vkr_depot_store_open(&store, root, vkr_depot_zstd_codec()));

  static uint8_t zeros[300000];
  VkrDepotId id;
  vkr_depot_id_of(zeros, sizeof(zeros), &id);
  VkrDepotId wrong = id;
  wrong.bytes[0] ^= 1u;
  assert(!vkr_depot_store_put(&store, &wrong, zeros, sizeof(zeros)));
  assert(vkr_depot_store_put(&store, &id, zeros, sizeof(zeros)));
  assert(vkr_depot_store_has(&store, &id));
  assert(!vkr_depot_store_has(&store, &wrong));

  uint8_t *stored = NULL;
  uint64_t stored_size = 0u;
  assert(vkr_depot_store_get_stored(&store, &id, &stored, &stored_size));
  assert(stored[0] == 1u && stored_size < sizeof(zeros) / 10u);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  assert(vkr_depot_store_get(&store, &id, &bytes, &size));
  assert(size == sizeof(zeros) && memcmp(bytes, zeros, size) == 0);
  free(bytes);

  /* A second store receives the representation; corruption is refused. */
  char other_root[VKR_DEPOT_PATH_CAPACITY];
  depot_test_dir("store_b", other_root, sizeof(other_root));
  VkrDepotStore other;
  assert(vkr_depot_store_open(&other, other_root, vkr_depot_zstd_codec()));
  stored[stored_size - 1u] ^= 0x40u;
  assert(!vkr_depot_store_put_stored(&other, &id, stored, stored_size));
  stored[stored_size - 1u] ^= 0x40u;
  assert(vkr_depot_store_put_stored(&other, &id, stored, stored_size));
  free(stored);

  /* Random data stays raw. */
  static uint8_t noise[100000];
  depot_test_random(noise, sizeof(noise));
  vkr_depot_id_of(noise, sizeof(noise), &id);
  assert(vkr_depot_store_put(&store, &id, noise, sizeof(noise)));
  assert(vkr_depot_store_get_stored(&store, &id, &stored, &stored_size));
  assert(stored[0] == 0u);
  free(stored);

  /* A file a crash left torn fails its read and is removed, so the object
     counts as missing and is fetched again. */
  char path[VKR_DEPOT_PATH_CAPACITY];
  vkr_depot_store_path(&store, &id, path, sizeof(path));
  assert(vkr_bakery_write_file_atomic(path, noise, 4096u));
  assert(vkr_depot_store_has(&store, &id));
  assert(!vkr_depot_store_get(&store, &id, &bytes, &size));
  assert(!vkr_depot_store_has(&store, &id));
  assert(vkr_depot_store_put(&store, &id, noise, sizeof(noise)));
  assert(vkr_depot_store_get(&store, &id, &bytes, &size));
  assert(size == sizeof(noise) && memcmp(bytes, noise, size) == 0);
  free(bytes);
  assert(vkr_bakery_remove_tree(root) && vkr_bakery_remove_tree(other_root));
  printf("  test_depot_store PASSED\n");
}

/* Fails when content-defined chunks leave their size bounds, or when an edit
   inside a large file moves more than the boundaries around it. */
static void test_depot_chunking(void) {
  printf("  Running test_depot_chunking...\n");
  const uint64_t size = 8u * 1024u * 1024u;
  uint8_t *data = malloc(size);
  depot_test_random(data, size);
  uint64_t boundaries[128];
  uint32_t count = 0u;
  for (uint64_t offset = 0u; offset < size;) {
    const uint32_t length =
        vkr_depot_chunk_length(data + offset, size - offset);
    assert(length >= Min((uint64_t)VKR_DEPOT_CHUNK_MIN, size - offset));
    assert(length <= VKR_DEPOT_CHUNK_MAX);
    offset += length;
    assert(count < ArrayCount(boundaries));
    boundaries[count++] = offset;
  }
  printf("    8 MiB in %u chunks (average %.0f KiB)\n", count,
         (double)size / count / 1024.0);
  assert(count >= 8u && count <= 128u);

  /* Overwrite 100 bytes in the middle: other boundaries stay put. */
  memset(data + size / 2u, 0x5a, 100u);
  uint32_t same = 0u;
  for (uint64_t offset = 0u; offset < size;) {
    offset += vkr_depot_chunk_length(data + offset, size - offset);
    for (uint32_t i = 0u; i < count; ++i) {
      if (boundaries[i] == offset) {
        ++same;
        break;
      }
    }
  }
  assert(same + 2u >= count);
  free(data);
  printf("  test_depot_chunking PASSED\n");
}

typedef struct ChangeCounts {
  uint32_t added;
  uint32_t modified;
  uint32_t deleted;
} ChangeCounts;

static bool8_t depot_test_count_change(void *context, const char *path,
                                       VkrDepotChange change) {
  (void)path;
  ChangeCounts *counts = context;
  if (change == VKR_DEPOT_CHANGE_ADDED) {
    counts->added += 1u;
  } else if (change == VKR_DEPOT_CHANGE_MODIFIED) {
    counts->modified += 1u;
  } else {
    counts->deleted += 1u;
  }
  return true_v;
}

/* Fails when a snapshot and a checkout into another working copy do not
   reproduce every file, when an unchanged snapshot changes the tree, or when
   the diff misses an addition, change or deletion. */
static void test_depot_workspace(void) {
  printf("  Running test_depot_workspace...\n");
  char root[VKR_DEPOT_PATH_CAPACITY];
  char copy_root[VKR_DEPOT_PATH_CAPACITY];
  depot_test_dir("work", root, sizeof(root));
  depot_test_dir("copy", copy_root, sizeof(copy_root));
  static uint8_t big[2500000];
  depot_test_random(big, sizeof(big));
  depot_test_write(root, "textures/big.bin", big, sizeof(big));
  depot_test_write(root, "scene.json", "{}", 2u);
  depot_test_write(root, "deep/a/b/c.txt", "nested", 6u);
  depot_test_write(root, "empty.dat", "", 0u);

  VkrDepotWorkspace workspace;
  assert(vkr_depot_workspace_create(&workspace, root, NULL,
                                    vkr_depot_zstd_codec()));
  VkrDepotId tree;
  VkrDepotFileList files;
  assert(vkr_depot_workspace_snapshot(&workspace, &tree, &files));
  assert(files.count == 4u);
  vkr_depot_file_list_free(&workspace.index);
  workspace.index = files;
  assert(vkr_depot_workspace_save(&workspace));

  /* An unchanged directory snapshots to the same tree. */
  VkrDepotId again;
  VkrDepotFileList again_files;
  assert(vkr_depot_workspace_snapshot(&workspace, &again, &again_files));
  assert(vkr_depot_id_equal(&tree, &again));
  vkr_depot_file_list_free(&again_files);

  /* Another working copy on the same store checks the tree out. */
  VkrDepotWorkspace copy;
  assert(vkr_depot_workspace_create(&copy, copy_root, workspace.store.root,
                                    vkr_depot_zstd_codec()));
  assert(vkr_depot_workspace_checkout(&copy, &tree, NULL, NULL));
  const char *paths[] = {"textures/big.bin", "scene.json", "deep/a/b/c.txt",
                         "empty.dat"};
  for (uint32_t i = 0u; i < ArrayCount(paths); ++i) {
    char a[VKR_DEPOT_PATH_CAPACITY];
    char b[VKR_DEPOT_PATH_CAPACITY];
    assert(vkr_bakery_path_join(a, sizeof(a), root, paths[i]));
    assert(vkr_bakery_path_join(b, sizeof(b), copy_root, paths[i]));
    assert(depot_test_files_equal(a, b));
  }

#if !defined(_WIN32)
  /* Checkout does not flush, so a crash can leave a file with its recorded
     size and time but other bytes; the next snapshot must still hash it. */
  char torn[VKR_DEPOT_PATH_CAPACITY];
  assert(vkr_bakery_path_join(torn, sizeof(torn), copy_root, "scene.json"));
  VkrBakeryStat written = {0};
  assert(vkr_bakery_stat(torn, &written));
  depot_test_write(copy_root, "scene.json", "[]", 2u);
  const struct timespec times[2] = {{.tv_sec = written.mtime_ns / 1000000000,
                                     .tv_nsec = written.mtime_ns % 1000000000},
                                    {.tv_sec = written.mtime_ns / 1000000000,
                                     .tv_nsec = written.mtime_ns % 1000000000}};
  assert(utimensat(AT_FDCWD, torn, times, 0) == 0);
  VkrBakeryStat tampered = {0};
  assert(vkr_bakery_stat(torn, &tampered) && tampered.size == written.size &&
         tampered.mtime_ns == written.mtime_ns);
  VkrDepotId copy_tree;
  VkrDepotFileList copy_files;
  assert(vkr_depot_workspace_snapshot(&copy, &copy_tree, &copy_files));
  ChangeCounts torn_counts = {0};
  assert(vkr_depot_file_list_diff(&copy.index, &copy_files,
                                  depot_test_count_change, &torn_counts));
  assert(torn_counts.modified == 1u && torn_counts.added == 0u &&
         torn_counts.deleted == 0u);
  vkr_depot_file_list_free(&copy_files);
#endif

  /* Edits show in the diff against the index. */
  depot_test_write(root, "scene.json", "{\"x\":1}", 7u);
  depot_test_write(root, "new.txt", "new", 3u);
  char removed[VKR_DEPOT_PATH_CAPACITY];
  assert(vkr_bakery_path_join(removed, sizeof(removed), root, "empty.dat"));
  assert(vkr_bakery_remove_file(removed));
  VkrDepotFileList edited;
  assert(vkr_depot_workspace_snapshot(&workspace, &again, &edited));
  ChangeCounts counts = {0};
  assert(vkr_depot_file_list_diff(&workspace.index, &edited,
                                  depot_test_count_change, &counts));
  assert(counts.added == 1u && counts.modified == 1u && counts.deleted == 1u);
  assert(!vkr_depot_id_equal(&tree, &again));

  /* Checking the old tree out again restores the files. */
  vkr_depot_file_list_free(&workspace.index);
  workspace.index = edited;
  assert(vkr_depot_workspace_checkout(&workspace, &tree, NULL, NULL));
  assert(vkr_bakery_is_file(removed));
  char added[VKR_DEPOT_PATH_CAPACITY];
  assert(vkr_bakery_path_join(added, sizeof(added), root, "new.txt"));
  assert(!vkr_bakery_is_file(added));

  vkr_depot_workspace_close(&copy);
  vkr_depot_workspace_close(&workspace);
  assert(vkr_bakery_remove_tree(root) && vkr_bakery_remove_tree(copy_root));
  printf("  test_depot_workspace PASSED\n");
}

// =============================================================================
// Client and server over the simulated network
// =============================================================================

typedef struct DepotWorld {
  VkrAllocator allocator;
  VkrDMemory memory;
  VkrNetSim *sim;
  uint64_t now;
  VkrNetCore *server_core;
  VkrNetSession *server_session;
  VkrDepotServer *server;
  VkrNetCore *client_core;
  VkrNetSession *client_session;
  VkrNetConnectionId connection;
  bool8_t connected;
  bool8_t closed;
  uint16_t close_code;
  VkrNetAddress server_address;
  uint8_t server_key[32];
  /* The credential each connect sends; empty for none. */
  char name[VKR_DEPOT_DISPLAY_NAME];
  /* Canceled once a fetch has received this many chunk bytes; zero never. */
  VkrDepotProgress *cancel_progress;
  uint64_t cancel_after_bytes;
} DepotWorld;

static void depot_world_accept(void *context, const VkrNetAddress *address,
                               const uint8_t peer_key[32],
                               const uint8_t *credential,
                               uint32_t credential_size,
                               VkrNetAcceptResult *out_result) {
  DepotWorld *world = context;
  vkr_depot_server_accept(world->server, address, peer_key, credential,
                          credential_size, out_result);
}

static bool8_t depot_world_pump(void *context, int32_t timeout_ms) {
  DepotWorld *world = context;
  /* One step of virtual time, at least the next event, at most the wait. */
  const uint64_t limit = world->now + (uint64_t)Max(timeout_ms, 1) * 1000u;
  uint64_t next = vkr_net_sim_next_time(world->sim);
  next = Clamp(next, world->now, limit);
  world->now = next == world->now ? world->now + 1u : next;
  vkr_net_sim_step(world->sim, world->now);
  VkrNetEvent event;
  while (vkr_net_core_poll(world->server_core, &event)) {
    if (!vkr_net_session_dispatch(world->server_session, &event, world->now)) {
      vkr_depot_server_on_event(world->server, &event);
    }
  }
  vkr_depot_server_update(world->server, world->now);
  while (vkr_net_core_poll(world->client_core, &event)) {
    if (vkr_net_session_dispatch(world->client_session, &event, world->now) ||
        event.connection != world->connection) {
      continue;
    }
    if (event.type == VKR_NET_EVENT_CONNECTED) {
      world->connected = true_v;
    } else if (event.type == VKR_NET_EVENT_CLOSED) {
      world->closed = true_v;
      world->close_code = event.code;
    }
  }
  vkr_net_core_flush(world->client_core);
  if (world->cancel_progress &&
      vkr_depot_progress_read(world->cancel_progress).bytes_done >=
          world->cancel_after_bytes) {
    vkr_depot_progress_cancel(world->cancel_progress);
  }
  return true_v;
}

static uint64_t depot_world_now(void *context) {
  return ((DepotWorld *)context)->now;
}

static void depot_world_init(DepotWorld *world, const char *store_root,
                             const VkrNetSimLink *link,
                             VkrNetKeyPair *client_keys) {
  MemZero(world, sizeof(*world));
  assert(vkr_dmemory_create(MB(16), GB(2), &world->memory));
  world->allocator = (VkrAllocator){.ctx = &world->memory};
  vkr_dmemory_allocator_create(&world->allocator);
  world->sim = vkr_net_sim_create(&world->allocator, 5u);

  VkrNetCoreConfig server_config = {.accept_incoming = true_v,
                                    .accept = depot_world_accept,
                                    .accept_context = world,
                                    .sent_packet_capacity = 8192u};
  assert(vkr_depot_server_key(store_root, &server_config.static_keys));
  VkrNetCoreConfig client_config = {.static_keys = *client_keys,
                                    .sent_packet_capacity = 8192u};
  world->server_core = vkr_net_core_create(&world->allocator, &server_config);
  world->client_core = vkr_net_core_create(&world->allocator, &client_config);
  world->server_session =
      vkr_net_session_create(&world->allocator, world->server_core);
  world->client_session =
      vkr_net_session_create(&world->allocator, world->client_core);
  const VkrDepotServerConfig depot_config = {.store_root = store_root,
                                             .codec = vkr_depot_zstd_codec()};
  world->server = vkr_depot_server_create(&world->allocator, world->server_core,
                                          world->server_session, &depot_config);
  assert(world->server);

  VkrNetAddress server_address, client_address;
  assert(vkr_net_address_parse("10.1.0.1:7320", 0u, &server_address));
  assert(vkr_net_address_parse("10.1.0.2:5000", 0u, &client_address));
  const uint32_t s =
      vkr_net_sim_add(world->sim, world->server_core, &server_address);
  const uint32_t c =
      vkr_net_sim_add(world->sim, world->client_core, &client_address);
  vkr_net_sim_set_link(world->sim, s, c, link);
  vkr_net_sim_set_link(world->sim, c, s, link);
  world->server_address = server_address;
  memcpy(world->server_key, server_config.static_keys.public_key, 32u);
  assert(vkr_net_core_connect(world->client_core, &server_address,
                              world->server_key, NULL, 0u, world->now,
                              &world->connection));
}

/* Connects the client again, sending `name` as the credential, to the
   server that kept running. */
static void depot_world_reconnect(DepotWorld *world, const char *name) {
  snprintf(world->name, sizeof(world->name), "%s", name);
  world->connected = false_v;
  world->closed = false_v;
  world->close_code = 0u;
  assert(vkr_net_core_connect(world->client_core, &world->server_address,
                              world->server_key, (const uint8_t *)world->name,
                              (uint32_t)strlen(world->name), world->now,
                              &world->connection));
}

/* Pumps until the client connection opens or closes. */
static void depot_world_settle(DepotWorld *world) {
  for (uint32_t i = 0u; i < 20000u && !world->connected && !world->closed;
       ++i) {
    (void)depot_world_pump(world, 10);
  }
}

static void depot_world_destroy(DepotWorld *world) {
  vkr_depot_server_destroy(world->server);
  vkr_net_session_destroy(world->client_session);
  vkr_net_session_destroy(world->server_session);
  vkr_net_core_destroy(world->client_core);
  vkr_net_core_destroy(world->server_core);
  vkr_net_sim_destroy(world->sim);
  vkr_dmemory_allocator_destroy(&world->allocator);
}

/* Connects and returns a client on `store`, or NULL when refused. */
static VkrDepotClient *depot_world_client(DepotWorld *world,
                                          const VkrDepotStore *store) {
  for (uint32_t i = 0u; i < 20000u && !world->connected; ++i) {
    (void)depot_world_pump(world, 10);
  }
  if (!world->connected) {
    return NULL;
  }
  const VkrDepotLink link = {.core = world->client_core,
                             .session = world->client_session,
                             .connection = world->connection,
                             .context = world,
                             .pump = depot_world_pump,
                             .now = depot_world_now};
  return vkr_depot_client_create(&link, store, 60000u);
}

static bool8_t depot_test_count_lock(void *context, const char *path,
                                     const uint8_t owner[32],
                                     uint64_t time_us) {
  (void)path;
  (void)owner;
  (void)time_us;
  *(uint32_t *)context += 1u;
  return true_v;
}

/* A commit of the working copy at `root` into its store. */
static void depot_test_commit(VkrDepotWorkspace *workspace, const char *text,
                              const VkrDepotId *parent, VkrDepotId *out_commit,
                              VkrDepotFileList *out_files) {
  VkrDepotId tree;
  assert(vkr_depot_workspace_snapshot(workspace, &tree, out_files));
  VkrDepotCommit commit = {.tree = tree,
                           .time_us = 1u,
                           .message = text,
                           .message_size = (uint32_t)strlen(text)};
  if (parent && !vkr_depot_id_zero(parent)) {
    commit.parents[0] = *parent;
    commit.parent_count = 1u;
  }
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  assert(vkr_depot_commit_encode(&commit, &bytes, &size));
  vkr_depot_id_of(bytes, size, out_commit);
  assert(vkr_depot_store_put(&workspace->store, out_commit, bytes, size));
  free(bytes);
}

/* Fails when an ignore pattern matches the wrong paths, or a snapshot keeps
   a file `.vkrdepotignore` names or drops one it does not. */
static void test_depot_ignore(void) {
  printf("  Running test_depot_ignore...\n");
  assert(vkr_depot_ignore_match("*.log", "a/b/run.log", false_v));
  assert(!vkr_depot_ignore_match("*.log", "a/b/run.logs", false_v));
  assert(vkr_depot_ignore_match(".vkreditor/jobs/", ".vkreditor/jobs", true_v));
  assert(
      !vkr_depot_ignore_match(".vkreditor/jobs/", ".vkreditor/jobs", false_v));
  assert(
      !vkr_depot_ignore_match(".vkreditor/jobs/", "x/.vkreditor/jobs", true_v));
  assert(vkr_depot_ignore_match("cache/", "deep/cache", true_v));
  assert(vkr_depot_ignore_match("**/build/*.tmp", "a/b/build/x.tmp", false_v));
  assert(vkr_depot_ignore_match("**/build/*.tmp", "build/x.tmp", false_v));
  assert(!vkr_depot_ignore_match("a/*.tmp", "a/b/x.tmp", false_v));
  assert(vkr_depot_ignore_match("/workspace.json", "workspace.json", false_v));

  char root[VKR_DEPOT_PATH_CAPACITY];
  depot_test_dir("ignore", root, sizeof(root));
  const char *rules =
      "# editor state\n.vkreditor/jobs/\n*.log\r\n\n  cache/ \n";
  depot_test_write(root, ".vkrdepotignore", rules, (uint64_t)strlen(rules));
  depot_test_write(root, "scene.json", "{}", 2u);
  depot_test_write(root, ".vkreditor/jobs/1/result.json", "{}", 2u);
  depot_test_write(root, ".vkreditor/projects/p/project.json", "{}", 2u);
  depot_test_write(root, "logs/run.log", "x", 1u);
  depot_test_write(root, "a/cache/blob.bin", "x", 1u);
  VkrDepotWorkspace workspace;
  assert(vkr_depot_workspace_create(&workspace, root, NULL,
                                    vkr_depot_zstd_codec()));
  VkrDepotId tree;
  VkrDepotFileList files;
  assert(vkr_depot_workspace_snapshot(&workspace, &tree, &files));
  assert(files.count == 3u);
  assert(vkr_depot_file_list_find(&files, ".vkrdepotignore"));
  assert(vkr_depot_file_list_find(&files, "scene.json"));
  assert(
      vkr_depot_file_list_find(&files, ".vkreditor/projects/p/project.json"));
  vkr_depot_file_list_free(&files);
  vkr_depot_workspace_close(&workspace);
  assert(vkr_bakery_remove_tree(root));
  printf("  test_depot_ignore PASSED\n");
}

/* Fails when a push over a lossy path does not reach the server whole, a
   fetch into an empty store does not reproduce every file, a second push
   resends unchanged objects, a stale compare-and-swap or a commit with
   missing objects moves the branch, a lock does not stop another key's
   change or lock, also when the paths differ only in case, or an unknown
   key gets in. */
static void test_depot_transfer(void) {
  printf("  Running test_depot_transfer...\n");
  char store_root[VKR_DEPOT_PATH_CAPACITY];
  char alice_root[VKR_DEPOT_PATH_CAPACITY];
  char bob_root[VKR_DEPOT_PATH_CAPACITY];
  depot_test_dir("server", store_root, sizeof(store_root));
  depot_test_dir("alice", alice_root, sizeof(alice_root));
  depot_test_dir("bob", bob_root, sizeof(bob_root));
  const VkrNetSimLink link = {.latency_us = 10000u,
                              .jitter_us = 3000u,
                              .loss = 0.05f,
                              .bandwidth = 50u * 1000u * 1000u};

  VkrNetKeyPair alice_keys, bob_keys, carol_keys;
  vkr_net_keypair_generate(&alice_keys);
  vkr_net_keypair_generate(&bob_keys);
  vkr_net_keypair_generate(&carol_keys);
  assert(vkr_depot_access_set(store_root, alice_keys.public_key,
                              VKR_DEPOT_ROLE_WRITE));
  assert(vkr_depot_access_set(store_root, bob_keys.public_key,
                              VKR_DEPOT_ROLE_WRITE));

  /* Alice pushes the first commit. */
  static uint8_t big[3000000];
  depot_test_random(big, sizeof(big));
  depot_test_write(alice_root, "textures/big.bin", big, sizeof(big));
  depot_test_write(alice_root, "scene.json", "{\"entities\":[]}", 15u);
  VkrDepotWorkspace alice;
  assert(vkr_depot_workspace_create(&alice, alice_root, NULL,
                                    vkr_depot_zstd_codec()));
  VkrDepotId first;
  VkrDepotFileList files;
  depot_test_commit(&alice, "first", NULL, &first, &files);
  vkr_depot_file_list_free(&files);

  DepotWorld world;
  depot_world_init(&world, store_root, &link, &alice_keys);
  VkrDepotClient *client = depot_world_client(&world, &alice.store);
  assert(client);
  VkrDepotTransferStats stats = {0};
  if (!vkr_depot_client_upload(client, &first, NULL, &stats)) {
    printf("    upload failed: %s\n", vkr_depot_client_error(client));
    assert(false && "first upload");
  }
  printf("    first push: %llu objects, %llu bytes\n",
         (unsigned long long)stats.objects,
         (unsigned long long)stats.stored_bytes);
  const VkrDepotId zero = {0};
  VkrDepotId current;
  assert(vkr_depot_client_update_ref(client, "main", &zero, &first, &current) ==
         VKR_DEPOT_REF_OK);
  /* A second push of the same commit finds nothing missing. */
  assert(vkr_depot_client_upload(client, &first, NULL, &stats));
  assert(stats.objects == 0u);
  /* A stale expectation does not move the branch. */
  assert(vkr_depot_client_update_ref(client, "main", &zero, &first, &current) ==
         VKR_DEPOT_REF_CONFLICT);
  assert(vkr_depot_id_equal(&current, &first));

  /* A commit whose tree never reached the server is incomplete. */
  depot_test_write(alice_root, "only_local.bin", "x", 1u);
  VkrDepotId unsent;
  depot_test_commit(&alice, "unsent", &first, &unsent, &files);
  vkr_depot_file_list_free(&files);
  assert(vkr_depot_client_update_ref(client, "main", &first, &unsent,
                                     &current) == VKR_DEPOT_REF_INCOMPLETE);
  /* Bob holds a lock on scene.json. */
  vkr_depot_client_destroy(client);
  depot_world_destroy(&world);

  depot_world_init(&world, store_root, &link, &bob_keys);
  VkrDepotWorkspace bob;
  assert(
      vkr_depot_workspace_create(&bob, bob_root, NULL, vkr_depot_zstd_codec()));
  client = depot_world_client(&world, &bob.store);
  assert(client);
  VkrDepotRef refs[4];
  uint32_t ref_count = 0u;
  assert(vkr_depot_client_refs(client, refs, 4u, &ref_count));
  assert(ref_count == 1u && strcmp(refs[0].name, "main") == 0 &&
         vkr_depot_id_equal(&refs[0].commit, &first));
  /* A sparse fetch brings the trees, which list every file, and no file;
     one file then arrives on its own and equals Alice's. */
  assert(vkr_depot_client_fetch_trees(client, &first, &stats));
  assert(stats.objects == 3u);
  uint8_t *sparse_bytes = NULL;
  uint64_t sparse_size = 0u;
  VkrDepotCommit sparse_commit;
  assert(vkr_depot_store_get(&bob.store, &first, &sparse_bytes, &sparse_size));
  assert(vkr_depot_commit_parse(sparse_bytes, sparse_size, &sparse_commit));
  free(sparse_bytes);
  VkrDepotFileList listed = {0};
  assert(vkr_depot_tree_files(&bob.store, &sparse_commit.tree, &listed));
  assert(listed.count == 2u &&
         strcmp(listed.entries[1].path, "textures/big.bin") == 0);
  const VkrDepotId big_blob = listed.entries[1].blob;
  vkr_depot_file_list_free(&listed);
  assert(!vkr_depot_store_has(&bob.store, &big_blob));
  assert(vkr_depot_client_fetch_files(client, &big_blob, 1u, &stats));
  assert(vkr_depot_store_has(&bob.store, &big_blob) && stats.objects > 1u);
  char sparse_a[VKR_DEPOT_PATH_CAPACITY];
  char sparse_b[VKR_DEPOT_PATH_CAPACITY];
  assert(vkr_bakery_path_join(sparse_a, sizeof(sparse_a), alice_root,
                              "textures/big.bin"));
  assert(vkr_bakery_path_join(sparse_b, sizeof(sparse_b), bob_root,
                              "sparse_big.bin"));
  assert(vkr_depot_write_blob(&bob.store, &big_blob, sparse_b));
  assert(depot_test_files_equal(sparse_a, sparse_b));
  assert(vkr_bakery_remove_file(sparse_b));
  assert(vkr_depot_client_fetch(client, &first, &stats));
  printf("    fetch: %llu objects, %llu bytes\n",
         (unsigned long long)stats.objects,
         (unsigned long long)stats.stored_bytes);
  uint8_t *commit_bytes = NULL;
  uint64_t commit_size = 0u;
  VkrDepotCommit parsed;
  assert(vkr_depot_store_get(&bob.store, &first, &commit_bytes, &commit_size));
  assert(vkr_depot_commit_parse(commit_bytes, commit_size, &parsed));
  assert(vkr_depot_workspace_checkout(&bob, &parsed.tree, NULL, NULL));
  free(commit_bytes);
  char a[VKR_DEPOT_PATH_CAPACITY];
  char b[VKR_DEPOT_PATH_CAPACITY];
  assert(vkr_bakery_path_join(a, sizeof(a), alice_root, "textures/big.bin"));
  assert(vkr_bakery_path_join(b, sizeof(b), bob_root, "textures/big.bin"));
  assert(depot_test_files_equal(a, b));
  uint8_t owner[32];
  /* Locked under another case: on macOS and Windows it is one file. */
  assert(vkr_depot_client_lock(client, "Scene.JSON", false_v, false_v, owner) ==
         VKR_DEPOT_LOCK_OK);
  vkr_depot_client_destroy(client);
  depot_world_destroy(&world);

  /* Alice's change to the locked file is refused; others pass. */
  depot_world_init(&world, store_root, &link, &alice_keys);
  client = depot_world_client(&world, &alice.store);
  assert(client);
  assert(vkr_depot_client_lock(client, "scene.json", true_v, false_v, owner) ==
         VKR_DEPOT_LOCK_HELD);
  assert(vkr_depot_client_lock(client, "SCENE.json", false_v, false_v, owner) ==
         VKR_DEPOT_LOCK_HELD);
  assert(memcmp(owner, bob_keys.public_key, 32u) == 0);
  depot_test_write(alice_root, "scene.json", "{\"entities\":[1]}", 16u);
  VkrDepotId touches_lock;
  depot_test_commit(&alice, "touch", &first, &touches_lock, &files);
  vkr_depot_file_list_free(&files);
  assert(vkr_depot_client_upload(client, &touches_lock, &first, &stats));
  assert(vkr_depot_client_update_ref(client, "main", &first, &touches_lock,
                                     &current) == VKR_DEPOT_REF_LOCKED);
  depot_test_write(alice_root, "scene.json", "{\"entities\":[]}", 15u);
  VkrDepotId allowed;
  depot_test_commit(&alice, "allowed", &first, &allowed, &files);
  vkr_depot_file_list_free(&files);
  assert(vkr_depot_client_upload(client, &allowed, &first, &stats));
  /* Only the new file's objects and the new trees travel. */
  assert(stats.objects <= 5u);
  assert(vkr_depot_client_update_ref(client, "main", &first, &allowed,
                                     &current) == VKR_DEPOT_REF_OK);
  vkr_depot_client_destroy(client);
  depot_world_destroy(&world);

  /* A key without a role is refused at the handshake. */
  depot_world_init(&world, store_root, &link, &carol_keys);
  for (uint32_t i = 0u; i < 2000u; ++i) {
    (void)depot_world_pump(&world, 10);
  }
  assert(!world.connected);
  depot_world_destroy(&world);

  vkr_depot_workspace_close(&bob);
  vkr_depot_workspace_close(&alice);
  assert(vkr_bakery_remove_tree(store_root));
  assert(vkr_bakery_remove_tree(alice_root));
  assert(vkr_bakery_remove_tree(bob_root));
  printf("  test_depot_transfer PASSED\n");
}

/* A working copy of `store_root`'s `commit`, fetched and checked out over
   the simulated network as `keys`. */
static void depot_test_checkout(const char *store_root, VkrNetKeyPair *keys,
                                VkrDepotWorkspace *workspace,
                                const VkrDepotId *commit) {
  const VkrNetSimLink link = {.latency_us = 2000u,
                              .bandwidth = 100u * 1000u * 1000u};
  DepotWorld world;
  depot_world_init(&world, store_root, &link, keys);
  VkrDepotClient *client = depot_world_client(&world, &workspace->store);
  assert(client);
  uint32_t kept = 0u;
  char error[256];
  assert(vkr_depot_fetch_checkout(workspace, client, commit,
                                  VKR_DEPOT_CONFLICT_FAIL, &kept, NULL, error,
                                  sizeof(error)));
  vkr_depot_client_destroy(client);
  depot_world_destroy(&world);
}

/* Uploads `commit` and moves main from `expected` to it. */
static void depot_test_push(const char *store_root, VkrNetKeyPair *keys,
                            VkrDepotWorkspace *workspace,
                            const VkrDepotId *expected,
                            const VkrDepotId *commit) {
  const VkrNetSimLink link = {.latency_us = 2000u,
                              .bandwidth = 100u * 1000u * 1000u};
  DepotWorld world;
  depot_world_init(&world, store_root, &link, keys);
  VkrDepotClient *client = depot_world_client(&world, &workspace->store);
  assert(client);
  VkrDepotTransferStats stats = {0};
  assert(vkr_depot_client_upload(client, commit, expected, &stats));
  VkrDepotId current;
  assert(vkr_depot_client_update_ref(client, "main", expected, commit,
                                     &current) == VKR_DEPOT_REF_OK);
  vkr_depot_client_destroy(client);
  depot_world_destroy(&world);
}

static bool8_t depot_test_file_is(const char *root, const char *relative,
                                  const char *text) {
  char path[VKR_DEPOT_PATH_CAPACITY];
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  const bool8_t ok = vkr_bakery_path_join(path, sizeof(path), root, relative) &&
                     vkr_bakery_read_file(path, 1u << 20, &bytes, &size) &&
                     size == strlen(text) && memcmp(bytes, text, size) == 0;
  free(bytes);
  return ok;
}

/* The change status shows for `path`, or 0 when it shows none. */
static uint32_t depot_test_status_of(VkrDepotWorkspace *workspace,
                                     const char *path) {
  VkrDepotChangeList changes;
  char error[256];
  assert(vkr_depot_status(workspace, &changes, error, sizeof(error)));
  uint32_t change = 0u;
  for (uint32_t i = 0u; i < changes.count; ++i) {
    if (strcmp(changes.entries[i].path, path) == 0) {
      change = changes.entries[i].change;
    }
  }
  vkr_depot_change_list_free(&changes);
  return change;
}

/* Fails when an unknown key's refusal is not recorded as a request with the
   name it sent, when a role given while the server runs does not let the
   same key in without a restart, when NONE leaves its live connection open
   or its request listed, or when a changed access file on disk is not
   picked up by the running server. */
static void test_depot_runtime_access(void) {
  printf("  Running test_depot_runtime_access...\n");
  char store_root[VKR_DEPOT_PATH_CAPACITY];
  char carol_root[VKR_DEPOT_PATH_CAPACITY];
  depot_test_dir("access", store_root, sizeof(store_root));
  depot_test_dir("carol", carol_root, sizeof(carol_root));
  VkrNetKeyPair alice_keys, carol_keys;
  vkr_net_keypair_generate(&alice_keys);
  vkr_net_keypair_generate(&carol_keys);
  assert(vkr_depot_access_set(store_root, alice_keys.public_key,
                              VKR_DEPOT_ROLE_ADMIN));
  VkrDepotWorkspace carol;
  assert(vkr_depot_workspace_create(&carol, carol_root, NULL,
                                    vkr_depot_zstd_codec()));
  const VkrNetSimLink link = {.latency_us = 2000u,
                              .bandwidth = 100u * 1000u * 1000u};
  DepotWorld world;
  depot_world_init(&world, store_root, &link, &carol_keys);
  depot_world_settle(&world);
  assert(world.closed && !world.connected);
  /* The anonymous first try, then two named ones: one request. */
  depot_world_reconnect(&world, "carol@laptop");
  depot_world_settle(&world);
  depot_world_reconnect(&world, "carol@laptop");
  depot_world_settle(&world);
  assert(world.closed && world.close_code == VKR_NET_CLOSE_ACCESS_DENIED);
  VkrDepotAccessRequest requests[VKR_DEPOT_REQUESTS_MAX];
  assert(vkr_depot_server_requests(world.server, requests,
                                   VKR_DEPOT_REQUESTS_MAX) == 1u);
  assert(memcmp(requests[0].key, carol_keys.public_key, 32u) == 0);
  assert(strcmp(requests[0].name, "carol@laptop") == 0);
  assert(requests[0].tries >= 3u);

  /* Allowed at runtime: the same key gets in on its next try. */
  assert(vkr_depot_server_set_role(world.server, carol_keys.public_key,
                                   VKR_DEPOT_ROLE_READ, world.now));
  assert(vkr_depot_server_requests(world.server, requests,
                                   VKR_DEPOT_REQUESTS_MAX) == 0u);
  VkrDepotAccessEntry entries[4];
  assert(vkr_depot_server_access(world.server, entries, 4u) == 2u);
  depot_world_reconnect(&world, "carol@laptop");
  VkrDepotClient *client = depot_world_client(&world, &carol.store);
  assert(client);
  VkrDepotRef refs[4];
  uint32_t ref_count = 0u;
  assert(vkr_depot_client_refs(client, refs, 4u, &ref_count));

  /* Denied: the live connection closes, a retry stays out of the list. */
  assert(vkr_depot_server_set_role(world.server, carol_keys.public_key,
                                   VKR_DEPOT_ROLE_NONE, world.now));
  depot_world_settle(&world);
  for (uint32_t i = 0u; i < 2000u && !world.closed; ++i) {
    (void)depot_world_pump(&world, 10);
  }
  assert(world.closed);
  depot_world_reconnect(&world, "carol@laptop");
  depot_world_settle(&world);
  assert(world.closed && !world.connected);
  assert(vkr_depot_server_requests(world.server, requests,
                                   VKR_DEPOT_REQUESTS_MAX) == 0u);

  /* `vkr_depot access` writes the file; the running server reloads it. */
  assert(vkr_depot_access_set(store_root, carol_keys.public_key,
                              VKR_DEPOT_ROLE_WRITE));
  for (uint32_t i = 0u; i < 150u; ++i) {
    (void)depot_world_pump(&world, 10);
  }
  depot_world_reconnect(&world, "carol@laptop");
  depot_world_settle(&world);
  assert(world.connected);
  vkr_depot_client_destroy(client);
  depot_world_destroy(&world);
  vkr_depot_workspace_close(&carol);
  assert(vkr_bakery_remove_tree(store_root));
  assert(vkr_bakery_remove_tree(carol_root));
  printf("  test_depot_runtime_access PASSED\n");
}

/* Fails when a conflicting path does not fail the checkout unchanged under
   FAIL, when TAKE_THEIRS keeps the local content or loses a local change
   the commit did not touch, or when KEEP_MINE overwrites the local content
   or leaves the base behind instead of showing the content as a change. */
static void test_depot_conflicts(void) {
  printf("  Running test_depot_conflicts...\n");
  char store_root[VKR_DEPOT_PATH_CAPACITY];
  char alice_root[VKR_DEPOT_PATH_CAPACITY];
  char fail_root[VKR_DEPOT_PATH_CAPACITY];
  char theirs_root[VKR_DEPOT_PATH_CAPACITY];
  char mine_root[VKR_DEPOT_PATH_CAPACITY];
  depot_test_dir("conflict_server", store_root, sizeof(store_root));
  depot_test_dir("conflict_alice", alice_root, sizeof(alice_root));
  depot_test_dir("conflict_fail", fail_root, sizeof(fail_root));
  depot_test_dir("conflict_theirs", theirs_root, sizeof(theirs_root));
  depot_test_dir("conflict_mine", mine_root, sizeof(mine_root));
  VkrNetKeyPair alice_keys, bob_keys;
  vkr_net_keypair_generate(&alice_keys);
  vkr_net_keypair_generate(&bob_keys);
  assert(vkr_depot_access_set(store_root, alice_keys.public_key,
                              VKR_DEPOT_ROLE_WRITE));
  assert(vkr_depot_access_set(store_root, bob_keys.public_key,
                              VKR_DEPOT_ROLE_WRITE));

  VkrDepotWorkspace alice;
  assert(vkr_depot_workspace_create(&alice, alice_root, NULL,
                                    vkr_depot_zstd_codec()));
  depot_test_write(alice_root, "a.txt", "base", 4u);
  depot_test_write(alice_root, "b.txt", "base", 4u);
  VkrDepotId first;
  VkrDepotId second;
  VkrDepotFileList files;
  const VkrDepotId zero = {0};
  depot_test_commit(&alice, "first", NULL, &first, &files);
  vkr_depot_file_list_free(&files);
  depot_test_push(store_root, &alice_keys, &alice, &zero, &first);

  /* Three working copies of the first commit change a.txt and b.txt. */
  const char *roots[3] = {fail_root, theirs_root, mine_root};
  VkrDepotWorkspace copies[3];
  for (uint32_t i = 0u; i < 3u; ++i) {
    assert(vkr_depot_workspace_create(&copies[i], roots[i], NULL,
                                      vkr_depot_zstd_codec()));
    depot_test_checkout(store_root, &bob_keys, &copies[i], &first);
    depot_test_write(roots[i], "a.txt", "mine", 4u);
    depot_test_write(roots[i], "b.txt", "local", 5u);
  }

  /* Alice changes a.txt only. */
  depot_test_write(alice_root, "a.txt", "theirs", 6u);
  depot_test_commit(&alice, "second", &first, &second, &files);
  vkr_depot_file_list_free(&files);
  depot_test_push(store_root, &alice_keys, &alice, &first, &second);

  const VkrDepotConflictPolicy policies[3] = {VKR_DEPOT_CONFLICT_FAIL,
                                              VKR_DEPOT_CONFLICT_TAKE_THEIRS,
                                              VKR_DEPOT_CONFLICT_KEEP_MINE};
  const VkrNetSimLink link = {.latency_us = 2000u,
                              .bandwidth = 100u * 1000u * 1000u};
  for (uint32_t i = 0u; i < 3u; ++i) {
    DepotWorld world;
    depot_world_init(&world, store_root, &link, &bob_keys);
    VkrDepotClient *client = depot_world_client(&world, &copies[i].store);
    assert(client);
    uint32_t kept = 0u;
    VkrDepotChangeList conflicts;
    char error[256];
    const bool8_t ok =
        vkr_depot_fetch_checkout(&copies[i], client, &second, policies[i],
                                 &kept, &conflicts, error, sizeof(error));
    assert(conflicts.count == 1u &&
           strcmp(conflicts.entries[0].path, "a.txt") == 0 &&
           conflicts.entries[0].change == VKR_DEPOT_CHANGE_MODIFIED);
    vkr_depot_change_list_free(&conflicts);
    vkr_depot_client_destroy(client);
    depot_world_destroy(&world);
    assert(depot_test_file_is(roots[i], "b.txt", "local"));
    if (policies[i] == VKR_DEPOT_CONFLICT_FAIL) {
      assert(!ok && vkr_depot_id_equal(&copies[i].base, &first));
      assert(depot_test_file_is(roots[i], "a.txt", "mine"));
      continue;
    }
    assert(ok && vkr_depot_id_equal(&copies[i].base, &second));
    assert(depot_test_status_of(&copies[i], "b.txt") ==
           VKR_DEPOT_CHANGE_MODIFIED);
    if (policies[i] == VKR_DEPOT_CONFLICT_TAKE_THEIRS) {
      assert(kept == 1u);
      assert(depot_test_file_is(roots[i], "a.txt", "theirs"));
      assert(depot_test_status_of(&copies[i], "a.txt") == 0u);
    } else {
      assert(kept == 2u);
      assert(depot_test_file_is(roots[i], "a.txt", "mine"));
      assert(depot_test_status_of(&copies[i], "a.txt") ==
             VKR_DEPOT_CHANGE_MODIFIED);
    }
  }
  for (uint32_t i = 0u; i < 3u; ++i) {
    vkr_depot_workspace_close(&copies[i]);
    assert(vkr_bakery_remove_tree(roots[i]));
  }
  vkr_depot_workspace_close(&alice);
  assert(vkr_bakery_remove_tree(store_root));
  assert(vkr_bakery_remove_tree(alice_root));
  printf("  test_depot_conflicts PASSED\n");
}

/* Fails when a cancel does not stop a fetch, when the canceled fetch
   leaves the working copy on another base or with files, or when a later
   fetch of the same commit cannot complete it, including over files a
   canceled checkout already wrote. */
static void test_depot_cancel(void) {
  printf("  Running test_depot_cancel...\n");
  char store_root[VKR_DEPOT_PATH_CAPACITY];
  char alice_root[VKR_DEPOT_PATH_CAPACITY];
  char bob_root[VKR_DEPOT_PATH_CAPACITY];
  depot_test_dir("cancel_server", store_root, sizeof(store_root));
  depot_test_dir("cancel_alice", alice_root, sizeof(alice_root));
  depot_test_dir("cancel_bob", bob_root, sizeof(bob_root));
  VkrNetKeyPair alice_keys, bob_keys;
  vkr_net_keypair_generate(&alice_keys);
  vkr_net_keypair_generate(&bob_keys);
  assert(vkr_depot_access_set(store_root, alice_keys.public_key,
                              VKR_DEPOT_ROLE_WRITE));
  assert(vkr_depot_access_set(store_root, bob_keys.public_key,
                              VKR_DEPOT_ROLE_READ));
  static uint8_t big[4000000];
  depot_test_random(big, sizeof(big));
  depot_test_write(alice_root, "textures/big.bin", big, sizeof(big));
  depot_test_write(alice_root, "scene.json", "{}", 2u);
  VkrDepotWorkspace alice;
  assert(vkr_depot_workspace_create(&alice, alice_root, NULL,
                                    vkr_depot_zstd_codec()));
  VkrDepotId commit;
  VkrDepotFileList files;
  const VkrDepotId zero = {0};
  depot_test_commit(&alice, "first", NULL, &commit, &files);
  vkr_depot_file_list_free(&files);
  depot_test_push(store_root, &alice_keys, &alice, &zero, &commit);

  VkrDepotWorkspace bob;
  assert(
      vkr_depot_workspace_create(&bob, bob_root, NULL, vkr_depot_zstd_codec()));
  VkrDepotProgress progress;
  vkr_depot_progress_reset(&progress);
  bob.progress = &progress;
  const VkrNetSimLink link = {.latency_us = 5000u,
                              .bandwidth = 20u * 1000u * 1000u};
  DepotWorld world;
  depot_world_init(&world, store_root, &link, &bob_keys);
  VkrDepotClient *client = depot_world_client(&world, &bob.store);
  assert(client);
  world.cancel_progress = &progress;
  world.cancel_after_bytes = 1000000u;
  uint32_t kept = 0u;
  char error[256];
  assert(!vkr_depot_fetch_checkout(&bob, client, &commit,
                                   VKR_DEPOT_CONFLICT_FAIL, &kept, NULL, error,
                                   sizeof(error)));
  assert(strcmp(error, "canceled") == 0);
  const VkrDepotProgressView view = vkr_depot_progress_read(&progress);
  assert(view.canceled && view.phase == VKR_DEPOT_PHASE_FETCH &&
         view.bytes_done < sizeof(big));
  assert(vkr_depot_id_zero(&bob.base) && bob.index.count == 0u);
  char path[VKR_DEPOT_PATH_CAPACITY];
  assert(vkr_bakery_path_join(path, sizeof(path), bob_root, "scene.json"));
  assert(!vkr_bakery_is_file(path));

  /* A canceled checkout leaves some of the commit's files written. */
  depot_test_write(bob_root, "scene.json", "{}", 2u);
  world.cancel_progress = NULL;
  vkr_depot_progress_reset(&progress);
  if (!vkr_depot_fetch_checkout(&bob, client, &commit, VKR_DEPOT_CONFLICT_FAIL,
                                &kept, NULL, error, sizeof(error))) {
    printf("    resumed fetch failed: %s\n", error);
    assert(false && "resumed fetch");
  }
  assert(vkr_depot_id_equal(&bob.base, &commit) && kept == 0u);
  assert(vkr_depot_progress_read(&progress).phase == VKR_DEPOT_PHASE_DONE);
  char a[VKR_DEPOT_PATH_CAPACITY];
  char b[VKR_DEPOT_PATH_CAPACITY];
  assert(vkr_bakery_path_join(a, sizeof(a), alice_root, "textures/big.bin"));
  assert(vkr_bakery_path_join(b, sizeof(b), bob_root, "textures/big.bin"));
  assert(depot_test_files_equal(a, b));
  vkr_depot_client_destroy(client);
  depot_world_destroy(&world);
  vkr_depot_workspace_close(&bob);
  vkr_depot_workspace_close(&alice);
  assert(vkr_bakery_remove_tree(store_root));
  assert(vkr_bakery_remove_tree(alice_root));
  assert(vkr_bakery_remove_tree(bob_root));
  printf("  test_depot_cancel PASSED\n");
}

#if defined(_WIN32)
#define DEPOT_TEST_HOME_VARIABLE "LOCALAPPDATA"
#else
#define DEPOT_TEST_HOME_VARIABLE "HOME"
#endif

static void depot_test_setenv(const char *name, const char *value) {
#if defined(_WIN32)
  assert(_putenv_s(name, value ? value : "") == 0);
#else
  if (value) {
    assert(setenv(name, value, 1) == 0);
  } else {
    assert(unsetenv(name) == 0);
  }
#endif
}

/* Fails when a key in the old cache folder is not carried to the user data
   folder, when the carried key differs (access lists naming it would stop
   matching), when the copy is readable by other users, or when a later run
   without the old file makes another key.
   Runs under a temporary home, so the user's real keys stay untouched. */
static void test_depot_identity_migration(void) {
  printf("  Running test_depot_identity_migration...\n");
  char home[VKR_DEPOT_PATH_CAPACITY];
  depot_test_dir("identity_home", home, sizeof(home));
  char saved_home[VKR_DEPOT_PATH_CAPACITY] = {0};
  char saved_identity[VKR_DEPOT_PATH_CAPACITY] = {0};
  const char *home_value = getenv(DEPOT_TEST_HOME_VARIABLE);
  const char *identity_value = getenv("VKR_DEPOT_IDENTITY");
  const bool8_t had_home = home_value != NULL;
  const bool8_t had_identity = identity_value && identity_value[0];
  snprintf(saved_home, sizeof(saved_home), "%s", had_home ? home_value : "");
  snprintf(saved_identity, sizeof(saved_identity), "%s",
           had_identity ? identity_value : "");
  depot_test_setenv(DEPOT_TEST_HOME_VARIABLE, home);
  depot_test_setenv("VKR_DEPOT_IDENTITY", NULL);

  char legacy_dir[VKR_DEPOT_PATH_CAPACITY];
  char legacy[VKR_DEPOT_PATH_CAPACITY];
  char path[VKR_DEPOT_PATH_CAPACITY];
  assert(vkr_bakery_user_cache_directory(legacy_dir, sizeof(legacy_dir)));
  assert(strncmp(legacy_dir, home, strlen(home)) == 0);
  assert(vkr_bakery_path_join(legacy, sizeof(legacy), legacy_dir,
                              "vkr/depot/identity.key"));
  assert(vkr_depot_identity_path(path, sizeof(path)));
  assert(strncmp(path, home, strlen(home)) == 0 && strcmp(path, legacy) != 0);
  VkrNetKeyPair old_keys;
  vkr_net_keypair_generate(&old_keys);
  char directory[VKR_DEPOT_PATH_CAPACITY];
  vkr_bakery_path_parent(directory, sizeof(directory), legacy);
  assert(vkr_bakery_make_directories(directory));
  assert(vkr_bakery_write_file_atomic(legacy, old_keys.secret_key,
                                      VKR_NET_KEY_SIZE));

  VkrNetKeyPair keys;
  assert(vkr_depot_identity(&keys, false_v));
  assert(memcmp(keys.public_key, old_keys.public_key, 32u) == 0);
  assert(vkr_bakery_is_file(path) && vkr_bakery_is_file(legacy));
#if !defined(_WIN32)
  /* The copy is the user's alone. */
  struct stat key_stat;
  assert(stat(path, &key_stat) == 0 && (key_stat.st_mode & 077) == 0);
#endif
  /* Cache cleanup removes the old file; the key stays the same. */
  assert(vkr_bakery_remove_file(legacy));
  assert(vkr_depot_identity(&keys, true_v));
  assert(memcmp(keys.public_key, old_keys.public_key, 32u) == 0);

  char name[VKR_DEPOT_DISPLAY_NAME];
  vkr_depot_display_name(name);
  assert(name[0] && strchr(name, '@'));

  depot_test_setenv(DEPOT_TEST_HOME_VARIABLE, had_home ? saved_home : NULL);
  depot_test_setenv("VKR_DEPOT_IDENTITY", had_identity ? saved_identity : NULL);
  assert(vkr_bakery_remove_tree(home));
  printf("  test_depot_identity_migration PASSED\n");
}

/* Fails when the service does not move to a free port beside a taken one,
   including a port another socket holds only for IPv4,
   when an unknown machine's connect does not report DENIED or leaves no
   request with its display name, when a role given through the service
   does not let it in on its next try, or when a browser does not see the
   service answer a unicast probe. Runs on loopback with a temporary
   identity, so the user's key stays untouched. */
static void test_depot_service(void) {
  printf("  Running test_depot_service...\n");
  char store_root[VKR_DEPOT_PATH_CAPACITY];
  char second_root[VKR_DEPOT_PATH_CAPACITY];
  char client_root[VKR_DEPOT_PATH_CAPACITY];
  char identity[VKR_DEPOT_PATH_CAPACITY];
  depot_test_dir("service", store_root, sizeof(store_root));
  depot_test_dir("service_b", second_root, sizeof(second_root));
  depot_test_dir("service_client", client_root, sizeof(client_root));
  assert(vkr_bakery_path_join(identity, sizeof(identity), client_root,
                              "identity.key"));
  char saved_identity[VKR_DEPOT_PATH_CAPACITY] = {0};
  const char *identity_value = getenv("VKR_DEPOT_IDENTITY");
  const bool8_t had_identity = identity_value && identity_value[0];
  snprintf(saved_identity, sizeof(saved_identity), "%s",
           had_identity ? identity_value : "");
  depot_test_setenv("VKR_DEPOT_IDENTITY", identity);

  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(16), GB(1), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  const uint16_t discovery_port = 7349u;
  VkrDepotServiceConfig config = {.store_root = store_root,
                                  .bind = "127.0.0.1:0",
                                  .discovery_port = discovery_port,
                                  .codec = vkr_depot_zstd_codec()};
  char error[256];
  VkrDepotService *first =
      vkr_depot_service_start(&allocator, &config, error, sizeof(error));
  assert(first);
  VkrDepotServiceInfo info;
  vkr_depot_service_info(first, &info);
  const uint16_t taken = info.address.port;
  char bind[64];
  snprintf(bind, sizeof(bind), "127.0.0.1:%u", taken);
  config.store_root = second_root;
  config.bind = bind;
  config.port_attempts = 4u;
  VkrDepotService *service =
      vkr_depot_service_start(&allocator, &config, error, sizeof(error));
  assert(service);
  vkr_depot_service_info(service, &info);
  assert(info.address.port > taken && info.address.port <= taken + 3u);
  assert(!info.discoverable);
  vkr_depot_service_stop(service);

  /* A dual-stack bind skips a port another socket holds for IPv4: IPv4
     peers would reach that socket instead. */
  VkrUdpSocket holder = {.handle = VKR_UDP_SOCKET_INVALID};
  const VkrNetAddress ipv4_any = vkr_net_address_any(VKR_NET_ADDRESS_IPV4, 0u);
  VkrNetAddress held;
  assert(vkr_udp_socket_open(&ipv4_any, NULL, &holder));
  assert(vkr_udp_socket_local_address(holder, &held));
  snprintf(bind, sizeof(bind), "[::]:%u", held.port);
  service = vkr_depot_service_start(&allocator, &config, error, sizeof(error));
  assert(service);
  vkr_depot_service_info(service, &info);
  assert(info.address.port > held.port && info.address.port <= held.port + 3u);
  vkr_depot_service_stop(service);
  vkr_udp_socket_close(&holder);

  /* A browser sees the first service on a unicast probe. */
  vkr_depot_service_info(first, &info);
  if (info.discoverable) {
    VkrDepotBrowser browser;
    assert(vkr_depot_browser_open(&browser, discovery_port));
    VkrNetAddress loopback;
    assert(vkr_net_address_parse("127.0.0.1", 0u, &loopback));
    assert(vkr_depot_browser_add_target(&browser, &loopback));
    for (uint32_t i = 0u; i < 40u && browser.found_count == 0u; ++i) {
      vkr_depot_browser_probe(&browser, false_v);
      vkr_platform_sleep(50u);
      (void)vkr_depot_browser_update(&browser);
    }
    assert(browser.found_count == 1u &&
           browser.found[0].address.port == info.address.port &&
           memcmp(browser.found[0].server_key, info.server_key, 32u) == 0);
    vkr_depot_browser_close(&browser);
    printf("    a browser found the service on UDP %u\n", discovery_port);
  } else {
    printf("    UDP %u is taken; the browser check is skipped\n",
           discovery_port);
  }

  /* An unknown machine asks, is let in through the service and retries. */
  char address[64];
  snprintf(address, sizeof(address), "127.0.0.1:%u", info.address.port);
  const VkrDepotConnectConfig connect = {.address = address,
                                         .server_key = info.server_key,
                                         .name = "dana@studio",
                                         .timeout_ms = 5000u};
  VkrDepotConnectStatus status = VKR_DEPOT_CONNECT_OK;
  assert(
      !vkr_depot_connect(&allocator, &connect, &status, error, sizeof(error)));
  assert(status == VKR_DEPOT_CONNECT_DENIED);
  VkrDepotAccessRequest requests[VKR_DEPOT_REQUESTS_MAX];
  assert(vkr_depot_service_requests(first, requests, VKR_DEPOT_REQUESTS_MAX) ==
         1u);
  assert(strcmp(requests[0].name, "dana@studio") == 0);
  assert(
      vkr_depot_service_set_role(first, requests[0].key, VKR_DEPOT_ROLE_READ));
  VkrDepotRef refs[4];
  uint32_t ref_count = 0u;
  assert(vkr_depot_branches(&allocator, &connect, refs, 4u, &ref_count, &status,
                            error, sizeof(error)));
  assert(status == VKR_DEPOT_CONNECT_OK && ref_count == 0u);
  vkr_depot_service_info(first, &info);
  assert(!info.failed);
  vkr_depot_service_stop(first);

  vkr_dmemory_allocator_destroy(&allocator);
  depot_test_setenv("VKR_DEPOT_IDENTITY", had_identity ? saved_identity : NULL);
  assert(vkr_bakery_remove_tree(store_root));
  assert(vkr_bakery_remove_tree(second_root));
  assert(vkr_bakery_remove_tree(client_root));
  printf("  test_depot_service PASSED\n");
}

typedef struct DepotRemoteReader {
  VkrDepotRemote *remote;
  const char *path;
  bool8_t ok;
} DepotRemoteReader;

static void *depot_remote_read(void *argument) {
  DepotRemoteReader *reader = argument;
  reader->ok = vkr_depot_remote_fetch(reader->remote, reader->path);
  return NULL;
}

/* Asset streaming over loopback (vkr_depot_remote.h). A working copy pushes
   its project and publishes its ignored cooked texture to the cooked side
   branch; a remote then lists both without fetching any file. Fails when a
   listed file does not arrive through a VFS read below the directory, when
   it arrives with other bytes, when readers asking at once on several
   threads see a failure, when an unlisted path is served, when a second
   remote fetches a file already present, or when publishing the same cooked
   files again commits. Then a member's Get: fails when the cooked texture
   does not arrive with the author's bytes, when a second fetch writes it
   again, or when, after the author changes its source without publishing,
   the old cooked file is written instead of left to the cook. */
static void test_depot_remote(void) {
  printf("  Running test_depot_remote...\n");
  char store_root[VKR_DEPOT_PATH_CAPACITY];
  char author_root[VKR_DEPOT_PATH_CAPACITY];
  char stream_root[VKR_DEPOT_PATH_CAPACITY];
  char key_root[VKR_DEPOT_PATH_CAPACITY];
  char identity[VKR_DEPOT_PATH_CAPACITY];
  depot_test_dir("remote_store", store_root, sizeof(store_root));
  depot_test_dir("remote_author", author_root, sizeof(author_root));
  depot_test_dir("remote_stream", stream_root, sizeof(stream_root));
  depot_test_dir("remote_key", key_root, sizeof(key_root));
  assert(vkr_bakery_path_join(identity, sizeof(identity), key_root,
                              "identity.key"));
  char saved_identity[VKR_DEPOT_PATH_CAPACITY] = {0};
  const char *identity_value = getenv("VKR_DEPOT_IDENTITY");
  const bool8_t had_identity = identity_value && identity_value[0];
  snprintf(saved_identity, sizeof(saved_identity), "%s",
           had_identity ? identity_value : "");
  depot_test_setenv("VKR_DEPOT_IDENTITY", identity);

  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(16), GB(1), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  const VkrDepotServiceConfig config = {.store_root = store_root,
                                        .bind = "127.0.0.1:0",
                                        .discovery_port = 7351u,
                                        .codec = vkr_depot_zstd_codec(),
                                        .open_access = true_v};
  char error[256];
  VkrDepotService *service =
      vkr_depot_service_start(&allocator, &config, error, sizeof(error));
  assert(service);
  VkrDepotServiceInfo info;
  vkr_depot_service_info(service, &info);
  char address[64];
  snprintf(address, sizeof(address), "127.0.0.1:%u", info.address.port);

  /* The author's project: a scene, a texture source and its cooked file,
     which the ignore rules keep out of the history. */
  static uint8_t cooked[600000];
  depot_test_random(cooked, sizeof(cooked));
  depot_test_write(author_root, ".vkrdepotignore", "*.vkt\n", 6u);
  depot_test_write(author_root, "scene.json", "{\"entities\":[]}", 15u);
  depot_test_write(author_root, "textures/wall.png", "png bytes", 9u);
  depot_test_write(author_root, "textures/wall.png.vkt", cooked,
                   sizeof(cooked));
  VkrDepotWorkspace author;
  assert(vkr_depot_workspace_create(&author, author_root, NULL,
                                    vkr_depot_zstd_codec()));
  const VkrDepotConnectConfig connect = {.address = address,
                                         .server_key = info.server_key,
                                         .store = &author.store,
                                         .timeout_ms = 5000u};
  VkrDepotConnection *connection =
      vkr_depot_connect(&allocator, &connect, NULL, error, sizeof(error));
  assert(connection);
  VkrDepotClient *client = vkr_depot_connection_client(connection);
  const VkrDepotId zero = {0};
  VkrDepotPendingCommit pending;
  assert(vkr_depot_commit_snapshot(&author, &zero, "project", &pending, error,
                                   sizeof(error)));
  assert(pending.files.count == 3u);
  if (!vkr_depot_commit_push(&author, client, "main", &zero, &zero, &pending,
                             error, sizeof(error))) {
    printf("    push failed: %s\n", error);
    assert(false && "push");
  }
  vkr_depot_pending_commit_free(&pending);
  VkrDepotId cooked_tree;
  uint32_t cooked_files = 0u;
  assert(vkr_depot_cooked_snapshot(&author, &cooked_tree, &cooked_files, error,
                                   sizeof(error)) &&
         cooked_files == 1u);
  VkrDepotCookedResult published;
  if (!vkr_depot_publish_cooked(&author, client, "main", &cooked_tree,
                                cooked_files, &published, error,
                                sizeof(error))) {
    printf("    cooked publish failed: %s\n", error);
    assert(false && "cooked publish");
  }
  assert(published.files == 1u && !published.unchanged);
  /* The same cooked files publish nothing; the index stays the project's. */
  VkrDepotCookedResult again;
  assert(vkr_depot_publish_cooked(&author, client, "main", &cooked_tree,
                                  cooked_files, &again, error, sizeof(error)));
  assert(again.unchanged &&
         vkr_depot_id_equal(&again.commit, &published.commit));
  assert(author.index.count == 3u);
  vkr_depot_disconnect(connection);

  /* A remote lists the four files and holds none of them yet. */
  const VkrDepotRemoteConfig remote_config = {.directory = stream_root,
                                              .address = address,
                                              .server_key = info.server_key,
                                              .codec = vkr_depot_zstd_codec()};
  VkrDepotRemote *remote =
      vkr_depot_remote_open(&allocator, &remote_config, error, sizeof(error));
  if (!remote) {
    printf("    remote failed: %s\n", error);
    assert(false && "remote open");
  }
  VkrDepotRemoteStats stats;
  vkr_depot_remote_stats(remote, &stats);
  assert(stats.listed == 4u && stats.present == 0u && stats.fetched == 0u &&
         vkr_depot_id_equal(&stats.cooked_commit, &published.commit));

  /* A read below the directory fetches the cooked file through the VFS. */
  assert(vkr_vfs_set_remote(stream_root, vkr_depot_remote_vfs_calls(), remote));
  char streamed[VKR_DEPOT_PATH_CAPACITY];
  char original[VKR_DEPOT_PATH_CAPACITY];
  assert(vkr_bakery_path_join(streamed, sizeof(streamed), stream_root,
                              "textures/wall.png.vkt"));
  assert(vkr_bakery_path_join(original, sizeof(original), author_root,
                              "textures/wall.png.vkt"));
  const FilePath streamed_path = {
      .path = {.str = (uint8_t *)streamed, .length = strlen(streamed)},
      .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_exists(&streamed_path));
  assert(depot_test_files_equal(streamed, original));
  char unlisted[VKR_DEPOT_PATH_CAPACITY];
  assert(vkr_bakery_path_join(unlisted, sizeof(unlisted), stream_root,
                              "textures/none.vkt"));
  const FilePath unlisted_path = {
      .path = {.str = (uint8_t *)unlisted, .length = strlen(unlisted)},
      .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(!file_exists(&unlisted_path));
  assert(vkr_vfs_set_remote(NULL, NULL, NULL));

  /* Readers asking at once on several threads all get their files. */
  DepotRemoteReader readers[4] = {
      {.remote = remote, .path = "scene.json"},
      {.remote = remote, .path = "textures/wall.png"},
      {.remote = remote, .path = "scene.json"},
      {.remote = remote, .path = ".vkrdepotignore"}};
  VkrThread threads[4];
  for (uint32_t i = 0u; i < 4u; ++i) {
    assert(vkr_thread_create(&allocator, &threads[i], depot_remote_read,
                             &readers[i]));
  }
  for (uint32_t i = 0u; i < 4u; ++i) {
    assert(vkr_thread_join(threads[i]));
    (void)vkr_thread_destroy(&allocator, &threads[i]);
    assert(readers[i].ok);
  }
  vkr_depot_remote_stats(remote, &stats);
  assert(stats.fetched == 4u && stats.failed == 0u);
  vkr_depot_remote_close(remote);

  /* A second remote finds the files in place and fetches none. */
  remote =
      vkr_depot_remote_open(&allocator, &remote_config, error, sizeof(error));
  assert(remote);
  vkr_depot_remote_stats(remote, &stats);
  assert(stats.present == 4u);
  assert(vkr_depot_remote_fetch(remote, "textures/wall.png.vkt"));
  vkr_depot_remote_stats(remote, &stats);
  assert(stats.fetched == 0u);
  vkr_depot_remote_close(remote);

  /* A member gets the project and takes the cooked texture. */
  char member_root[VKR_DEPOT_PATH_CAPACITY];
  depot_test_dir("remote_member", member_root, sizeof(member_root));
  const VkrDepotCloneConfig clone = {.address = address,
                                     .server_key = info.server_key,
                                     .directory = member_root,
                                     .branch = "main",
                                     .codec = vkr_depot_zstd_codec()};
  VkrDepotCloneResult cloned = {0};
  assert(vkr_depot_clone(&allocator, &clone, &cloned, error, sizeof(error)));
  VkrDepotWorkspace member;
  assert(
      vkr_depot_workspace_open(&member, member_root, vkr_depot_zstd_codec()));
  VkrDepotConnectConfig member_connect = {.address = address,
                                          .server_key = info.server_key,
                                          .store = &member.store,
                                          .timeout_ms = 5000u};
  connection = vkr_depot_connect(&allocator, &member_connect, NULL, error,
                                 sizeof(error));
  assert(connection);
  client = vkr_depot_connection_client(connection);
  VkrDepotCookedFetch fetched;
  if (!vkr_depot_fetch_cooked(&member, client, "main", &fetched, error,
                              sizeof(error))) {
    printf("    cooked fetch failed: %s\n", error);
    assert(false && "cooked fetch");
  }
  assert(fetched.listed == 1u && fetched.written == 1u && fetched.stale == 0u &&
         fetched.present == 0u);
  char member_cooked[VKR_DEPOT_PATH_CAPACITY];
  assert(vkr_bakery_path_join(member_cooked, sizeof(member_cooked), member_root,
                              "textures/wall.png.vkt"));
  assert(depot_test_files_equal(member_cooked, original));
  VkrDepotFileList cache = {0};
  assert(vkr_depot_cooked_cache_load(&member, &cache) && cache.count == 1u);
  vkr_depot_file_list_free(&cache);
  assert(vkr_depot_fetch_cooked(&member, client, "main", &fetched, error,
                                sizeof(error)));
  assert(fetched.present == 1u && fetched.written == 0u);
  vkr_depot_disconnect(connection);

  /* The author changes the texture's source and publishes no cooked file:
     the member's next cooked fetch leaves that texture to its cook. */
  depot_test_write(author_root, "textures/wall.png", "new png bytes", 13u);
  connection =
      vkr_depot_connect(&allocator, &connect, NULL, error, sizeof(error));
  assert(connection);
  client = vkr_depot_connection_client(connection);
  const VkrDepotId author_base = author.base;
  assert(vkr_depot_commit_snapshot(&author, &author_base, "new wall", &pending,
                                   error, sizeof(error)));
  assert(vkr_depot_commit_push(&author, client, "main", &author_base,
                               &author_base, &pending, error, sizeof(error)));
  vkr_depot_pending_commit_free(&pending);
  vkr_depot_disconnect(connection);
  assert(vkr_bakery_remove_file(member_cooked));
  connection = vkr_depot_connect(&allocator, &member_connect, NULL, error,
                                 sizeof(error));
  assert(connection);
  client = vkr_depot_connection_client(connection);
  uint32_t kept = 0u;
  assert(vkr_depot_fetch_checkout(&member, client, &author.base,
                                  VKR_DEPOT_CONFLICT_FAIL, &kept, NULL, error,
                                  sizeof(error)));
  assert(vkr_depot_fetch_cooked(&member, client, "main", &fetched, error,
                                sizeof(error)));
  assert(fetched.stale == 1u && fetched.written == 0u &&
         !vkr_bakery_is_file(member_cooked));
  vkr_depot_disconnect(connection);
  vkr_depot_workspace_close(&member);

  vkr_vfs_unmount_all();
  vkr_depot_workspace_close(&author);
  vkr_depot_service_stop(service);
  vkr_dmemory_allocator_destroy(&allocator);
  depot_test_setenv("VKR_DEPOT_IDENTITY", had_identity ? saved_identity : NULL);
  assert(vkr_bakery_remove_tree(store_root));
  assert(vkr_bakery_remove_tree(author_root));
  assert(vkr_bakery_remove_tree(stream_root));
  assert(vkr_bakery_remove_tree(key_root));
  assert(vkr_bakery_remove_tree(member_root));
  printf("  test_depot_remote PASSED\n");
}

/* Decoded bytes of the chunks of `data` that hold bytes of [offset,
   offset + size), from the same chunking the store uses. */
static uint64_t depot_test_range_chunk_bytes(const uint8_t *data,
                                             uint64_t data_size,
                                             uint64_t offset, uint64_t size) {
  uint64_t covered = 0u;
  for (uint64_t start = 0u; start < data_size;) {
    const uint32_t length =
        vkr_depot_chunk_length(data + start, data_size - start);
    if (start + length > offset && start < offset + size) {
      covered += length;
    }
    start += length;
  }
  return covered;
}

/* Part of a streamed file over loopback (vkr_depot_remote_read). Fails when
   a read brings chunks outside its range, returns other bytes than the
   file's, writes the file, accepts a range past the end or an unlisted
   path; when a prefetch does not make the file whole and equal to the
   original, or downloads again the chunks the reads brought; or when a read
   of a present file goes to the server. */
static void test_depot_remote_ranges(void) {
  printf("  Running test_depot_remote_ranges...\n");
  char store_root[VKR_DEPOT_PATH_CAPACITY];
  char author_root[VKR_DEPOT_PATH_CAPACITY];
  char stream_root[VKR_DEPOT_PATH_CAPACITY];
  char key_root[VKR_DEPOT_PATH_CAPACITY];
  char identity[VKR_DEPOT_PATH_CAPACITY];
  depot_test_dir("ranges_store", store_root, sizeof(store_root));
  depot_test_dir("ranges_author", author_root, sizeof(author_root));
  depot_test_dir("ranges_stream", stream_root, sizeof(stream_root));
  depot_test_dir("ranges_key", key_root, sizeof(key_root));
  assert(vkr_bakery_path_join(identity, sizeof(identity), key_root,
                              "identity.key"));
  char saved_identity[VKR_DEPOT_PATH_CAPACITY] = {0};
  const char *identity_value = getenv("VKR_DEPOT_IDENTITY");
  const bool8_t had_identity = identity_value && identity_value[0];
  snprintf(saved_identity, sizeof(saved_identity), "%s",
           had_identity ? identity_value : "");
  depot_test_setenv("VKR_DEPOT_IDENTITY", identity);

  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(16), GB(1), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  const VkrDepotServiceConfig config = {.store_root = store_root,
                                        .bind = "127.0.0.1:0",
                                        .discovery_port = 7352u,
                                        .codec = vkr_depot_zstd_codec(),
                                        .open_access = true_v};
  char error[256];
  VkrDepotService *service =
      vkr_depot_service_start(&allocator, &config, error, sizeof(error));
  assert(service);
  VkrDepotServiceInfo info;
  vkr_depot_service_info(service, &info);
  char address[64];
  snprintf(address, sizeof(address), "127.0.0.1:%u", info.address.port);

  /* Random bytes do not compress, so stored sizes follow decoded sizes, and
     3 MB of them make about a dozen chunks. */
  static uint8_t big[3000000];
  depot_test_random(big, sizeof(big));
  depot_test_write(author_root, "big.bin", big, sizeof(big));
  depot_test_write(author_root, "small.txt", "small", 5u);
  VkrDepotWorkspace author;
  assert(vkr_depot_workspace_create(&author, author_root, NULL,
                                    vkr_depot_zstd_codec()));
  const VkrDepotConnectConfig connect = {.address = address,
                                         .server_key = info.server_key,
                                         .store = &author.store,
                                         .timeout_ms = 5000u};
  VkrDepotConnection *connection =
      vkr_depot_connect(&allocator, &connect, NULL, error, sizeof(error));
  assert(connection);
  const VkrDepotId zero = {0};
  VkrDepotPendingCommit pending;
  assert(vkr_depot_commit_snapshot(&author, &zero, "ranges", &pending, error,
                                   sizeof(error)));
  if (!vkr_depot_commit_push(&author, vkr_depot_connection_client(connection),
                             "main", &zero, &zero, &pending, error,
                             sizeof(error))) {
    printf("    push failed: %s\n", error);
    assert(false && "push");
  }
  vkr_depot_pending_commit_free(&pending);
  vkr_depot_disconnect(connection);

  const VkrDepotRemoteConfig remote_config = {.directory = stream_root,
                                              .address = address,
                                              .server_key = info.server_key,
                                              .codec = vkr_depot_zstd_codec()};
  VkrDepotRemote *remote =
      vkr_depot_remote_open(&allocator, &remote_config, error, sizeof(error));
  if (!remote) {
    printf("    remote failed: %s\n", error);
    assert(false && "remote open");
  }
  uint64_t listed_size = 0u;
  assert(vkr_depot_remote_pending(remote, "big.bin", &listed_size) &&
         listed_size == sizeof(big));
  assert(!vkr_depot_remote_pending(remote, "none.bin", NULL));

  /* The head of the file arrives alone, with the original's bytes. */
  static uint8_t part[400000];
  assert(vkr_depot_remote_read(remote, "big.bin", 0u, 200000u, part));
  assert(MemCompare(part, big, 200000u) == 0);
  VkrDepotRemoteStats stats;
  vkr_depot_remote_stats(remote, &stats);
  const uint64_t head_chunks =
      depot_test_range_chunk_bytes(big, sizeof(big), 0u, 200000u);
  assert(stats.reads == 1u && stats.fetched == 0u);
  assert(stats.read_bytes >= head_chunks &&
         stats.read_bytes < head_chunks + KB(16));
  assert(head_chunks < sizeof(big) / 2u);
  assert(vkr_depot_remote_pending(remote, "big.bin", NULL));
  char streamed[VKR_DEPOT_PATH_CAPACITY];
  assert(
      vkr_bakery_path_join(streamed, sizeof(streamed), stream_root, "big.bin"));
  assert(!vkr_bakery_is_file(streamed));

  /* A range in the middle, and ranges a read must refuse. */
  assert(vkr_depot_remote_read(remote, "big.bin", 1500000u, 300000u, part));
  assert(MemCompare(part, big + 1500000u, 300000u) == 0);
  assert(
      !vkr_depot_remote_read(remote, "big.bin", sizeof(big) - 10u, 20u, part));
  assert(!vkr_depot_remote_read(remote, "none.bin", 0u, 4u, part));
  vkr_depot_remote_stats(remote, &stats);
  const uint64_t read_chunks =
      head_chunks +
      depot_test_range_chunk_bytes(big, sizeof(big), 1500000u, 300000u);
  assert(stats.reads == 2u && stats.read_bytes < read_chunks + KB(16));

  /* A prefetch makes the file whole and downloads only the other chunks. */
  vkr_depot_remote_prefetch(remote, "big.bin");
  for (uint32_t wait = 0u;
       wait < 1000u && vkr_depot_remote_arrivals(remote) == 0u; ++wait) {
    vkr_platform_sleep(10u);
  }
  assert(vkr_depot_remote_arrivals(remote) == 1u);
  assert(!vkr_depot_remote_pending(remote, "big.bin", NULL));
  char original[VKR_DEPOT_PATH_CAPACITY];
  assert(
      vkr_bakery_path_join(original, sizeof(original), author_root, "big.bin"));
  assert(depot_test_files_equal(streamed, original));
  vkr_depot_remote_stats(remote, &stats);
  assert(stats.prefetched == 1u && stats.fetched == 1u);
  assert(stats.stored_bytes < sizeof(big) - read_chunks + KB(16));
  assert(stats.stored_bytes + stats.read_bytes >= sizeof(big));

  /* A present file reads from the disk. */
  assert(vkr_depot_remote_read(remote, "big.bin", 2000000u, 1000u, part));
  assert(MemCompare(part, big + 2000000u, 1000u) == 0);
  vkr_depot_remote_stats(remote, &stats);
  assert(stats.reads == 2u);
  vkr_depot_remote_close(remote);

  vkr_depot_workspace_close(&author);
  vkr_depot_service_stop(service);
  vkr_dmemory_allocator_destroy(&allocator);
  depot_test_setenv("VKR_DEPOT_IDENTITY", had_identity ? saved_identity : NULL);
  assert(vkr_bakery_remove_tree(store_root));
  assert(vkr_bakery_remove_tree(author_root));
  assert(vkr_bakery_remove_tree(stream_root));
  assert(vkr_bakery_remove_tree(key_root));
  printf("  test_depot_remote_ranges PASSED\n");
}

/* Stores a tree of `count` entries into `store`. */
static void depot_test_tree(const VkrDepotStore *store,
                            VkrDepotTreeEntry *entries, uint32_t count,
                            VkrDepotId *out_tree) {
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  assert(vkr_depot_tree_encode(entries, count, &bytes, &size));
  vkr_depot_id_of(bytes, size, out_tree);
  assert(vkr_depot_store_put(store, out_tree, bytes, size));
  free(bytes);
}

/* Fails when a tree nested past VKR_DEPOT_TREE_DEPTH_MAX, or one subtree
   named at so many paths that its files pass VKR_DEPOT_TREE_BYTES_MAX, is
   read instead of refused: either would exhaust a thread's stack or a
   disk on checkout. */
static void test_depot_tree_limits(void) {
  printf("  Running test_depot_tree_limits...\n");
  char root[VKR_DEPOT_PATH_CAPACITY];
  depot_test_dir("limits", root, sizeof(root));
  VkrDepotStore store;
  assert(vkr_depot_store_open(&store, root, NULL));
  VkrDepotTreeEntry leaf = {.name = "f.bin",
                            .name_size = 5,
                            .kind = VKR_DEPOT_ENTRY_FILE,
                            .size = 1u};
  VkrDepotId tree;
  depot_test_tree(&store, &leaf, 1u, &tree);
  VkrDepotId at_limit = {0};
  for (uint32_t depth = 1u; depth <= VKR_DEPOT_TREE_DEPTH_MAX + 1u; ++depth) {
    VkrDepotTreeEntry folder = {
        .name = "d", .name_size = 1, .kind = VKR_DEPOT_ENTRY_TREE, .id = tree};
    depot_test_tree(&store, &folder, 1u, &tree);
    if (depth == VKR_DEPOT_TREE_DEPTH_MAX) {
      at_limit = tree;
    }
  }
  VkrDepotFileList files;
  assert(vkr_depot_tree_files(&store, &at_limit, &files) && files.count == 1u);
  vkr_depot_file_list_free(&files);
  assert(!vkr_depot_tree_files(&store, &tree, &files));

  /* Each level names the level below twice: 2^8 files of 4 GiB. */
  leaf.size = VKR_DEPOT_FILE_MAX;
  depot_test_tree(&store, &leaf, 1u, &tree);
  for (uint32_t level = 0u; level < 8u; ++level) {
    VkrDepotTreeEntry twice[2] = {
        {.name = "a", .name_size = 1, .kind = VKR_DEPOT_ENTRY_TREE, .id = tree},
        {.name = "b", .name_size = 1, .kind = VKR_DEPOT_ENTRY_TREE, .id = tree},
    };
    depot_test_tree(&store, twice, 2u, &tree);
  }
  assert(!vkr_depot_tree_files(&store, &tree, &files));
  assert(vkr_bakery_remove_tree(root));
  printf("  test_depot_tree_limits PASSED\n");
}

/* Fails when a checkout deletes a file outside the working copy that a
   planted state names, writes a path its ignore rules match or removes an
   ignored local file, lets a pulled `.vkrdepotignore` hide a tracked file's
   edit from the next snapshot, or loses a file renamed only in case. */
static void test_depot_checkout_safety(void) {
  printf("  Running test_depot_checkout_safety...\n");
  char source_root[VKR_DEPOT_PATH_CAPACITY];
  char root[VKR_DEPOT_PATH_CAPACITY];
  depot_test_dir("safety_source", source_root, sizeof(source_root));
  depot_test_dir("safety_copy", root, sizeof(root));
  VkrDepotWorkspace source;
  assert(vkr_depot_workspace_create(&source, source_root, NULL, NULL));
  VkrDepotWorkspace copy;
  assert(vkr_depot_workspace_create(&copy, root, source.store.root, NULL));

  /* A state that tracks a path outside the working copy. */
  char victim[VKR_DEPOT_PATH_CAPACITY];
  assert(vkr_bakery_path_join(victim, sizeof(victim), root,
                              "../safety_victim.txt"));
  assert(vkr_bakery_write_file_atomic(victim, "keep", 4u));
  const VkrDepotId zero = {0};
  assert(vkr_depot_file_list_add(&copy.index, "../safety_victim.txt", 4u, 0,
                                 &zero));
  vkr_depot_file_list_sort(&copy.index);
  assert(vkr_depot_workspace_save(&copy));
  vkr_depot_workspace_close(&copy);
  assert(vkr_depot_workspace_open(&copy, root, NULL));
  assert(copy.index.count == 0u);

  /* The source's tree carries a path the copy ignores. */
  depot_test_write(source_root, "Foo.png", "image", 5u);
  depot_test_write(source_root, "notes.txt", "notes", 5u);
  depot_test_write(source_root, "secret/lib.dylib", "native", 6u);
  VkrDepotId first;
  VkrDepotFileList files;
  assert(vkr_depot_workspace_snapshot(&source, &first, &files));
  vkr_depot_file_list_free(&files);
  depot_test_write(root, ".vkrdepotignore", "secret/\n", 8u);
  depot_test_write(root, "secret/keep.bin", "mine", 4u);
  uint32_t skipped = 0u;
  assert(vkr_depot_workspace_checkout(&copy, &first, NULL, &skipped));
  assert(skipped == 1u);
  assert(depot_test_file_is(root, "secret/keep.bin", "mine"));
  char planted[VKR_DEPOT_PATH_CAPACITY];
  assert(
      vkr_bakery_path_join(planted, sizeof(planted), root, "secret/lib.dylib"));
  assert(!vkr_bakery_is_file(planted));
  assert(vkr_bakery_is_file(victim));
  assert(depot_test_file_is(root, "Foo.png", "image"));

  /* A rule that matches a tracked file does not hide its edit. */
  depot_test_write(root, ".vkrdepotignore", "secret/\nnotes.txt\n", 18u);
  depot_test_write(root, "notes.txt", "edited", 6u);
  assert(depot_test_status_of(&copy, "notes.txt") == VKR_DEPOT_CHANGE_MODIFIED);
  depot_test_write(root, "notes.txt", "notes", 5u);

  /* Foo.png becomes foo.png: one file on macOS and Windows. */
  char old_name[VKR_DEPOT_PATH_CAPACITY];
  assert(
      vkr_bakery_path_join(old_name, sizeof(old_name), source_root, "Foo.png"));
  assert(vkr_bakery_remove_file(old_name));
  depot_test_write(source_root, "foo.png", "image", 5u);
  VkrDepotId second;
  assert(vkr_depot_workspace_snapshot(&source, &second, &files));
  vkr_depot_file_list_free(&files);
  assert(vkr_depot_workspace_checkout(&copy, &second, NULL, NULL));
  assert(depot_test_file_is(root, "foo.png", "image"));

  vkr_depot_workspace_close(&copy);
  vkr_depot_workspace_close(&source);
  assert(vkr_bakery_remove_file(victim));
  assert(vkr_bakery_remove_tree(root));
  assert(vkr_bakery_remove_tree(source_root));
  printf("  test_depot_checkout_safety PASSED\n");
}

/* Fails when the server moves a branch to a commit whose tree names the
   depot's state folder, which every working copy that pulls it would let
   overwrite its access list, or when a page of locks with long paths does
   not reach the client. */
static void test_depot_server_refusals(void) {
  printf("  Running test_depot_server_refusals...\n");
  char store_root[VKR_DEPOT_PATH_CAPACITY];
  depot_test_dir("refusals", store_root, sizeof(store_root));
  VkrNetKeyPair keys;
  vkr_net_keypair_generate(&keys);
  assert(
      vkr_depot_access_set(store_root, keys.public_key, VKR_DEPOT_ROLE_WRITE));
  VkrDepotStore store;
  assert(vkr_depot_store_open(&store, store_root, vkr_depot_zstd_codec()));
  /* A tree with `.vkrdepoX`, patched to `.vkrdepot` after encoding. */
  VkrDepotTreeEntry entry = {.name = ".vkrdepoX",
                             .name_size = 9,
                             .kind = VKR_DEPOT_ENTRY_FILE,
                             .size = 0u};
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  assert(vkr_depot_tree_encode(&entry, 1u, &bytes, &size));
  uint8_t *letter = memchr(bytes, 'X', size);
  assert(letter);
  *letter = 't';
  VkrDepotId tree;
  vkr_depot_id_of(bytes, size, &tree);
  assert(vkr_depot_store_put(&store, &tree, bytes, size));
  free(bytes);
  VkrDepotCommit commit = {.tree = tree, .message = "x", .message_size = 1u};
  assert(vkr_depot_commit_encode(&commit, &bytes, &size));
  VkrDepotId commit_id;
  vkr_depot_id_of(bytes, size, &commit_id);
  assert(vkr_depot_store_put(&store, &commit_id, bytes, size));
  free(bytes);

  const VkrNetSimLink link = {.latency_us = 2000u,
                              .bandwidth = 100u * 1000u * 1000u};
  DepotWorld world;
  depot_world_init(&world, store_root, &link, &keys);
  VkrDepotClient *client = depot_world_client(&world, &store);
  assert(client);
  const VkrDepotId zero = {0};
  VkrDepotId current;
  assert(vkr_depot_client_update_ref(client, "main", &zero, &commit_id,
                                     &current) == VKR_DEPOT_REF_INVALID);

  /* A full page of locks whose paths are as long as allowed. */
  char path[VKR_DEPOT_LOCK_PATH_MAX];
  for (uint32_t i = 0u; i < VKR_DEPOT_LOCKS_PAGE + 2u; ++i) {
    memset(path, 'a', 250u);
    path[250] = '/';
    snprintf(path + 251, sizeof(path) - 251u, "%03u", i);
    memset(path + 254, 'b', 250u);
    path[504] = '\0';
    uint8_t owner[32];
    assert(vkr_depot_client_lock(client, path, false_v, false_v, owner) ==
           VKR_DEPOT_LOCK_OK);
  }
  uint32_t listed = 0u;
  assert(vkr_depot_client_locks(client, depot_test_count_lock, &listed));
  assert(listed == VKR_DEPOT_LOCKS_PAGE + 2u);
  vkr_depot_client_destroy(client);
  depot_world_destroy(&world);
  assert(vkr_bakery_remove_tree(store_root));
  printf("  test_depot_server_refusals PASSED\n");
}

/* Fails when one network address can hold more than two pending requests
   and push other machines' requests out, or when a key its owner refused
   comes back into the list after enough other keys asked. */
static void test_depot_access_requests(void) {
  printf("  Running test_depot_access_requests...\n");
  char store_root[VKR_DEPOT_PATH_CAPACITY];
  depot_test_dir("requests", store_root, sizeof(store_root));
  VkrNetKeyPair keys;
  vkr_net_keypair_generate(&keys);
  const VkrNetSimLink link = {.latency_us = 2000u};
  DepotWorld world;
  depot_world_init(&world, store_root, &link, &keys);
  VkrNetAddress flooder;
  VkrNetAddress other;
  assert(vkr_net_address_parse("10.9.0.1:4000", 0u, &flooder));
  assert(vkr_net_address_parse("10.9.0.2:4000", 0u, &other));
  uint8_t key[32] = {0};
  VkrNetAcceptResult result;
  key[0] = 200u;
  MemZero(&result, sizeof(result));
  vkr_depot_server_accept(world.server, &other, key, (const uint8_t *)"o", 1u,
                          &result);
  assert(result.code == VKR_NET_CLOSE_ACCESS_DENIED);
  for (uint32_t i = 0u; i < 20u; ++i) {
    key[0] = (uint8_t)i;
    flooder.port = (uint16_t)(4000u + i);
    MemZero(&result, sizeof(result));
    vkr_depot_server_accept(world.server, &flooder, key, (const uint8_t *)"f",
                            1u, &result);
  }
  VkrDepotAccessRequest requests[VKR_DEPOT_REQUESTS_MAX];
  uint32_t count =
      vkr_depot_server_requests(world.server, requests, VKR_DEPOT_REQUESTS_MAX);
  assert(count == 3u);
  bool8_t other_kept = false_v;
  for (uint32_t i = 0u; i < count; ++i) {
    other_kept = other_kept || requests[i].key[0] == 200u;
  }
  assert(other_kept);

  /* Refused, then more keys than the list holds ask: it stays out. */
  key[0] = 200u;
  assert(vkr_depot_server_set_role(world.server, key, VKR_DEPOT_ROLE_NONE,
                                   world.now));
  for (uint32_t i = 0u; i < 2u * VKR_DEPOT_REQUESTS_MAX; ++i) {
    uint8_t asker[32] = {0};
    asker[0] = (uint8_t)(100u + i);
    VkrNetAddress address;
    char text[32];
    snprintf(text, sizeof(text), "10.8.0.%u:4000", i + 1u);
    assert(vkr_net_address_parse(text, 0u, &address));
    MemZero(&result, sizeof(result));
    vkr_depot_server_accept(world.server, &address, asker, (const uint8_t *)"n",
                            1u, &result);
  }
  MemZero(&result, sizeof(result));
  vkr_depot_server_accept(world.server, &other, key, (const uint8_t *)"o", 1u,
                          &result);
  assert(result.code == VKR_NET_CLOSE_ACCESS_DENIED);
  count =
      vkr_depot_server_requests(world.server, requests, VKR_DEPOT_REQUESTS_MAX);
  for (uint32_t i = 0u; i < count; ++i) {
    assert(requests[i].key[0] != 200u);
  }
  depot_world_destroy(&world);
  assert(vkr_bakery_remove_tree(store_root));
  printf("  test_depot_access_requests PASSED\n");
}

bool32_t run_depot_tests(void) {
  /* Unbuffered, so a stopped run shows the test it stopped in. */
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("--- Starting asset depot tests ---\n");
  assert(vkr_net_crypto_init());
  test_depot_objects();
  test_depot_store();
  test_depot_chunking();
  test_depot_workspace();
  test_depot_ignore();
  test_depot_transfer();
  test_depot_runtime_access();
  test_depot_conflicts();
  test_depot_cancel();
  test_depot_identity_migration();
  test_depot_service();
  test_depot_remote();
  test_depot_remote_ranges();
  test_depot_tree_limits();
  test_depot_checkout_safety();
  test_depot_server_refusals();
  test_depot_access_requests();
  printf("--- Asset depot tests completed ---\n");
  return true;
}
