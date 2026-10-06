#include "core/vkr_threads.h"
#include "filesystem/filesystem.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_local_socket.h"
#include "platform/vkr_platform.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

bool32_t run_local_socket_tests(void);

#define LOCAL_SOCKET_TEST_PATH 512u
/* Larger than the editor's 1 MiB line limit. */
#define LOCAL_SOCKET_TEST_LINE (MB(1) + KB(512))
/* "сокет" in UTF-8: a non-ASCII directory name on every host. */
#define LOCAL_SOCKET_TEST_UTF8_NAME "\xD1\x81\xD0\xBE\xD0\xBA\xD0\xB5\xD1\x82"

static uint32_t g_local_socket_test_counter = 0u;

/* A fresh socket path in the real per-user directory under the host's
   temporary directory. */
static void local_socket_test_path(const char *label, char *out) {
  char name[128];
  snprintf(name, sizeof(name), "test-%s-%u-%u.sock", label,
           vkr_platform_get_process_id(), ++g_local_socket_test_counter);
  assert(vkr_local_socket_user_path(name, out, LOCAL_SOCKET_TEST_PATH));
}

static bool8_t local_socket_test_file_exists(const char *path) {
  const FilePath file = {
      .path = string8_create_from_cstr((const uint8_t *)path, strlen(path)),
      .type = FILE_PATH_TYPE_ABSOLUTE};
  return file_exists(&file);
}

static void test_local_socket_user_directory(void) {
  printf("  Running test_local_socket_user_directory...\n");
  char directory[LOCAL_SOCKET_TEST_PATH];
  char temp[LOCAL_SOCKET_TEST_PATH];
  assert(vkr_local_socket_user_directory(directory, sizeof(directory)));
  assert(
      vkr_platform_user_directory(VKR_PLATFORM_USER_TEMP, temp, sizeof(temp)));
  /* The directory is `<temp>/vkr` and a second call accepts it. */
  const uint64_t temp_length = strlen(temp);
  assert(strncmp(directory, temp, temp_length) == 0);
  assert(strcmp(directory + temp_length, "/vkr") == 0);
  assert(vkr_local_socket_user_directory(directory, sizeof(directory)));
  bool8_t non_ascii = false_v;
  for (const char *c = directory; *c; ++c) {
    non_ascii = non_ascii || (uint8_t)*c >= 0x80u;
  }
  printf("    user directory %s (%s)\n", directory,
         non_ascii ? "non-ASCII UTF-8" : "ASCII");
#if defined(_WIN32)
  assert(vkr_local_socket_user_id() != 0u);
#endif
  printf("  test_local_socket_user_directory PASSED\n");
}

static void test_local_socket_listen_accept_close(void) {
  printf("  Running test_local_socket_listen_accept_close...\n");
  char path[LOCAL_SOCKET_TEST_PATH];
  local_socket_test_path("accept", path);
  VkrLocalSocket listener = VKR_LOCAL_SOCKET_INVALID;
  assert(vkr_local_socket_listen(path, 4, &listener) ==
         VKR_LOCAL_SOCKET_LISTEN_OK);
  assert(local_socket_test_file_exists(path));
  assert(vkr_local_socket_set_nonblocking(listener));

  /* No client waits yet. */
  VkrLocalSocket server = VKR_LOCAL_SOCKET_INVALID;
  assert(vkr_local_socket_accept(listener, &server) ==
         VKR_LOCAL_SOCKET_WOULD_BLOCK);
  assert(server == VKR_LOCAL_SOCKET_INVALID);

  VkrLocalSocket client = VKR_LOCAL_SOCKET_INVALID;
  assert(vkr_local_socket_connect(path, &client));
  VkrLocalSocketPoll waiting = {.socket = listener};
  assert(vkr_local_socket_poll(&waiting, 1u, 5000) == 1 && waiting.readable);
  assert(vkr_local_socket_accept(listener, &server) == VKR_LOCAL_SOCKET_OK);

  /* A short request crosses in both directions. */
  static const char request[] = "{\"v\":1,\"id\":1,\"op\":\"ping\"}\n";
  assert(vkr_local_socket_send_all(client, request, sizeof(request) - 1u));
  char received[64] = {0};
  uint64_t got = 0u;
  assert(vkr_local_socket_recv(server, received, sizeof(received), &got) ==
         VKR_LOCAL_SOCKET_OK);
  assert(got == sizeof(request) - 1u);
  assert(MemCompare(received, request, got) == 0);
  assert(vkr_local_socket_send_all(server, "ok\n", 3u));
  assert(vkr_local_socket_recv(client, received, sizeof(received), &got) ==
             VKR_LOCAL_SOCKET_OK &&
         got == 3u && MemCompare(received, "ok\n", 3u) == 0);

  /* A nonblocking socket with nothing to read would block; after the peer
     closes, the read reports the close. */
  assert(vkr_local_socket_set_nonblocking(server));
  assert(vkr_local_socket_recv(server, received, sizeof(received), &got) ==
         VKR_LOCAL_SOCKET_WOULD_BLOCK);
  vkr_local_socket_close(client);
  VkrLocalSocketPoll hangup = {.socket = server};
  assert(vkr_local_socket_poll(&hangup, 1u, 5000) == 1 && hangup.readable);
  assert(vkr_local_socket_recv(server, received, sizeof(received), &got) ==
         VKR_LOCAL_SOCKET_CLOSED);
  vkr_local_socket_close(server);

  /* Closing the listener leaves its file, as on POSIX; the owner removes
     it. */
  vkr_local_socket_close(listener);
  assert(local_socket_test_file_exists(path));
  assert(vkr_local_socket_remove_path(path));
  assert(!local_socket_test_file_exists(path));
  assert(vkr_local_socket_remove_path(path));
  printf("  test_local_socket_listen_accept_close PASSED\n");
}

static void test_local_socket_large_line(void) {
  printf("  Running test_local_socket_large_line...\n");
  char path[LOCAL_SOCKET_TEST_PATH];
  local_socket_test_path("large", path);
  VkrLocalSocket listener = VKR_LOCAL_SOCKET_INVALID;
  assert(vkr_local_socket_listen(path, 1, &listener) ==
         VKR_LOCAL_SOCKET_LISTEN_OK);
  VkrLocalSocket client = VKR_LOCAL_SOCKET_INVALID;
  VkrLocalSocket server = VKR_LOCAL_SOCKET_INVALID;
  assert(vkr_local_socket_connect(path, &client));
  assert(vkr_local_socket_accept(listener, &server) == VKR_LOCAL_SOCKET_OK);
  assert(vkr_local_socket_set_nonblocking(client));
  assert(vkr_local_socket_set_nonblocking(server));

  uint8_t *line = malloc(LOCAL_SOCKET_TEST_LINE);
  uint8_t *arrived = malloc(LOCAL_SOCKET_TEST_LINE);
  assert(line && arrived);
  for (uint64_t i = 0u; i + 1u < LOCAL_SOCKET_TEST_LINE; ++i) {
    line[i] = (uint8_t)('a' + (i * 7u + i / 4093u) % 26u);
  }
  line[LOCAL_SOCKET_TEST_LINE - 1u] = '\n';

  /* One thread alternates partial sends and reads, as the editor does with
     its nonblocking clients: a send that fills the socket buffer returns
     part of the line or would block, and the rest follows later. */
  uint64_t sent_total = 0u;
  uint64_t received_total = 0u;
  uint32_t full_buffers = 0u;
  uint32_t reads = 0u;
  while (received_total < LOCAL_SOCKET_TEST_LINE) {
    if (sent_total < LOCAL_SOCKET_TEST_LINE) {
      uint64_t sent = 0u;
      const VkrLocalSocketStatus status =
          vkr_local_socket_send(client, line + sent_total,
                                LOCAL_SOCKET_TEST_LINE - sent_total, &sent);
      assert(status == VKR_LOCAL_SOCKET_OK ||
             status == VKR_LOCAL_SOCKET_WOULD_BLOCK);
      full_buffers += status == VKR_LOCAL_SOCKET_WOULD_BLOCK ? 1u : 0u;
      sent_total += sent;
    }
    /* Reads take at most 64 KiB, as the editor reserves per read. */
    uint64_t got = 0u;
    const VkrLocalSocketStatus status = vkr_local_socket_recv(
        server, arrived + received_total,
        Min(LOCAL_SOCKET_TEST_LINE - received_total, (uint64_t)KB(64)), &got);
    assert(status == VKR_LOCAL_SOCKET_OK ||
           status == VKR_LOCAL_SOCKET_WOULD_BLOCK);
    if (status == VKR_LOCAL_SOCKET_OK) {
      received_total += got;
      reads++;
    } else {
      VkrLocalSocketPoll readable = {.socket = server};
      (void)vkr_local_socket_poll(&readable, 1u, 10);
    }
  }
  assert(sent_total == LOCAL_SOCKET_TEST_LINE);
  assert(received_total == LOCAL_SOCKET_TEST_LINE);
  assert(MemCompare(arrived, line, LOCAL_SOCKET_TEST_LINE) == 0);
  assert(reads >= LOCAL_SOCKET_TEST_LINE / KB(64));
  printf("    %llu bytes in %u reads; the send buffer filled %u times\n",
         (unsigned long long)received_total, reads, full_buffers);

  free(line);
  free(arrived);
  vkr_local_socket_close(client);
  vkr_local_socket_close(server);
  vkr_local_socket_close(listener);
  assert(vkr_local_socket_remove_path(path));
  printf("  test_local_socket_large_line PASSED\n");
}

static void test_local_socket_stale_and_live(void) {
  printf("  Running test_local_socket_stale_and_live...\n");
  char path[LOCAL_SOCKET_TEST_PATH];
  local_socket_test_path("stale", path);
  VkrLocalSocket first = VKR_LOCAL_SOCKET_INVALID;
  assert(vkr_local_socket_listen(path, 1, &first) ==
         VKR_LOCAL_SOCKET_LISTEN_OK);

  /* A live listener keeps its path. */
  VkrLocalSocket second = VKR_LOCAL_SOCKET_INVALID;
  assert(vkr_local_socket_listen(path, 1, &second) ==
         VKR_LOCAL_SOCKET_LISTEN_IN_USE);
  assert(second == VKR_LOCAL_SOCKET_INVALID);
  VkrLocalSocket client = VKR_LOCAL_SOCKET_INVALID;
  assert(vkr_local_socket_connect(path, &client));
  vkr_local_socket_close(client);

  /* A file nobody answers is stale and is replaced. */
  vkr_local_socket_close(first);
  assert(local_socket_test_file_exists(path));
  assert(!vkr_local_socket_connect(path, &client));
  assert(vkr_local_socket_listen(path, 1, &second) ==
         VKR_LOCAL_SOCKET_LISTEN_OK);
  assert(vkr_local_socket_connect(path, &client));
  VkrLocalSocket server = VKR_LOCAL_SOCKET_INVALID;
  assert(vkr_local_socket_accept(second, &server) == VKR_LOCAL_SOCKET_OK);
  vkr_local_socket_close(server);
  vkr_local_socket_close(client);
  vkr_local_socket_close(second);
  assert(vkr_local_socket_remove_path(path));

  /* No listener and no file: connecting fails. */
  assert(!vkr_local_socket_connect(path, &client));
  assert(client == VKR_LOCAL_SOCKET_INVALID);
  printf("  test_local_socket_stale_and_live PASSED\n");
}

typedef struct LocalSocketTestWaker {
  VkrLocalSocket socket;
  uint32_t delay_ms;
} LocalSocketTestWaker;

static void *local_socket_test_wake(void *context) {
  LocalSocketTestWaker *waker = context;
  vkr_platform_sleep(waker->delay_ms);
  const char byte = 1;
  uint64_t sent = 0u;
  (void)vkr_local_socket_send(waker->socket, &byte, 1u, &sent);
  return NULL;
}

static void test_local_socket_pair_wakes_poll(void) {
  printf("  Running test_local_socket_pair_wakes_poll...\n");
  Arena *arena = arena_create(MB(1), KB(64));
  VkrAllocator allocator = {.ctx = arena};
  vkr_allocator_arena(&allocator);

  VkrLocalSocket pair[2] = {VKR_LOCAL_SOCKET_INVALID, VKR_LOCAL_SOCKET_INVALID};
  assert(vkr_local_socket_pair(pair));
  assert(vkr_local_socket_set_nonblocking(pair[0]));
  VkrLocalSocketPoll wake = {.socket = pair[0]};
  assert(vkr_local_socket_poll(&wake, 1u, 0) == 0 && !wake.readable);

  /* Another thread's byte ends a long wait early. */
  LocalSocketTestWaker waker = {.socket = pair[1], .delay_ms = 50u};
  VkrThread thread = NULL;
  const float64_t started = vkr_platform_get_absolute_time();
  assert(
      vkr_thread_create(&allocator, &thread, local_socket_test_wake, &waker));
  assert(vkr_local_socket_poll(&wake, 1u, 10000) == 1 && wake.readable);
  const float64_t waited = vkr_platform_get_absolute_time() - started;
  assert(vkr_thread_join(thread));
  assert(waited < 5.0);
  char drain[8];
  uint64_t got = 0u;
  assert(vkr_local_socket_recv(pair[0], drain, sizeof(drain), &got) ==
             VKR_LOCAL_SOCKET_OK &&
         got == 1u);
  assert(vkr_local_socket_recv(pair[0], drain, sizeof(drain), &got) ==
         VKR_LOCAL_SOCKET_WOULD_BLOCK);
  printf("    woken after %.3f s\n", waited);

  vkr_local_socket_close(pair[0]);
  vkr_local_socket_close(pair[1]);
  arena_destroy(arena);
  printf("  test_local_socket_pair_wakes_poll PASSED\n");
}

static void local_socket_test_remove_directory(const char *path) {
#if defined(_WIN32)
  wchar_t wide[LOCAL_SOCKET_TEST_PATH];
  if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide,
                          (int)LOCAL_SOCKET_TEST_PATH)) {
    (void)RemoveDirectoryW(wide);
  }
#else
  (void)rmdir(path);
#endif
}

/* The socket file appears at the UTF-8 path the file system API names, so
   `sun_path` is read as UTF-8 (on Windows, not in the ANSI code page). */
static void test_local_socket_non_ascii_path(void) {
  printf("  Running test_local_socket_non_ascii_path...\n");
  char directory[LOCAL_SOCKET_TEST_PATH];
  assert(vkr_local_socket_user_directory(directory, sizeof(directory)));
  char child[LOCAL_SOCKET_TEST_PATH];
  snprintf(child, sizeof(child), "%s/" LOCAL_SOCKET_TEST_UTF8_NAME "-%u",
           directory, vkr_platform_get_process_id());
  const FilePath child_path = {
      .path = string8_create_from_cstr((const uint8_t *)child, strlen(child)),
      .type = FILE_PATH_TYPE_ABSOLUTE};
  assert(file_create_directory(&child_path));
  char path[LOCAL_SOCKET_TEST_PATH];
  snprintf(path, sizeof(path), "%s/s.sock", child);
  assert(vkr_local_socket_path_fits(path));

  VkrLocalSocket listener = VKR_LOCAL_SOCKET_INVALID;
  assert(vkr_local_socket_listen(path, 1, &listener) ==
         VKR_LOCAL_SOCKET_LISTEN_OK);
  assert(local_socket_test_file_exists(path));
  VkrLocalSocket client = VKR_LOCAL_SOCKET_INVALID;
  VkrLocalSocket server = VKR_LOCAL_SOCKET_INVALID;
  assert(vkr_local_socket_connect(path, &client));
  assert(vkr_local_socket_accept(listener, &server) == VKR_LOCAL_SOCKET_OK);
  assert(vkr_local_socket_send_all(client, "x", 1u));
  char byte = 0;
  uint64_t got = 0u;
  assert(vkr_local_socket_recv(server, &byte, 1u, &got) ==
             VKR_LOCAL_SOCKET_OK &&
         got == 1u && byte == 'x');
  printf("    listened on %s\n", path);

  vkr_local_socket_close(client);
  vkr_local_socket_close(server);
  vkr_local_socket_close(listener);
  assert(vkr_local_socket_remove_path(path));
  assert(!local_socket_test_file_exists(path));
  local_socket_test_remove_directory(child);
  assert(!file_exists(&child_path));
  printf("  test_local_socket_non_ascii_path PASSED\n");
}

static void test_local_socket_path_limit(void) {
  printf("  Running test_local_socket_path_limit...\n");
  char directory[LOCAL_SOCKET_TEST_PATH];
  assert(vkr_local_socket_user_directory(directory, sizeof(directory)));
  char path[LOCAL_SOCKET_TEST_PATH];
  const int written =
      snprintf(path, sizeof(path), "%s/%0*u.sock", directory, 120, 7u);
  assert(written > 0 && (uint64_t)written < sizeof(path));
  assert(!vkr_local_socket_path_fits(path));
  VkrLocalSocket socket = VKR_LOCAL_SOCKET_INVALID;
  assert(vkr_local_socket_listen(path, 1, &socket) ==
         VKR_LOCAL_SOCKET_LISTEN_TOO_LONG);
  assert(!vkr_local_socket_connect(path, &socket));
  assert(socket == VKR_LOCAL_SOCKET_INVALID);
  char name[160];
  snprintf(name, sizeof(name), "%0*u.sock", 120, 7u);
  assert(!vkr_local_socket_user_path(name, path, sizeof(path)));
  printf("  test_local_socket_path_limit PASSED\n");
}

bool32_t run_local_socket_tests(void) {
  printf("--- Starting Local Socket Tests ---\n");
  test_local_socket_user_directory();
  test_local_socket_listen_accept_close();
  test_local_socket_large_line();
  test_local_socket_stale_and_live();
  test_local_socket_pair_wakes_poll();
  test_local_socket_non_ascii_path();
  test_local_socket_path_limit();
  printf("--- Local Socket Tests Completed ---\n");
  return true;
}
