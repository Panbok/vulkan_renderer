#include "vkr_bakery_commands.h"

#include "core/vkr_threads.h"
#include "platform/vkr_local_socket.h"
#include "platform/vkr_platform.h"
#include "vkr_bakery_buffer.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#endif

/* `vkr_bakery serve`: one long-lived process per workspace or checkout.
 *
 * Clients speak newline-delimited JSON over a local stream socket, protocol
 * version 1:
 *   {"v":1,"id":N,"req":"run","argv":[...],"priority":"interactive"}
 *   {"v":1,"id":N,"req":"watch","paths":[...],"argv":[...]}   argv optional
 *   {"v":1,"id":N,"req":"unwatch","watch":W}
 *   {"v":1,"id":N,"req":"cancel","target":N}
 *   {"v":1,"id":N,"req":"ping"} / {"v":1,"id":N,"req":"shutdown"}
 * A run executes one subcommand (cook, build, shaders, materials, status,
 * inspect, explain, gc) in this process; each of its event lines reaches the
 * client with "req":N added, then {"ev":"reply","req":N,"exit":code}. A watch
 * reports {"ev":"changed","watch":W,"paths":[...]} after its files settle and,
 * with argv, runs it in the background with "watch":W on its events.
 * Commands run one at a time; interactive runs start before background ones.
 * The daemon exits on `shutdown`, SIGINT/SIGTERM, or after --idle-exit seconds
 * without clients. */

#define VKR_SERVE_PROTOCOL 1u
#define VKR_SERVE_MAX_CLIENTS 16u
#define VKR_SERVE_MAX_JOBS 64u
#define VKR_SERVE_MAX_WATCHES 32u
#define VKR_SERVE_MAX_WATCH_PATHS 16u
#define VKR_SERVE_MAX_ARGUMENTS 48u
#define VKR_SERVE_MAX_CHANGED 32u
#define VKR_SERVE_MAX_REQUEST_BYTES (256u * 1024u)
/* File systems report one save as several events; a watch waits this long
   after the last change before it reports and rebuilds. */
#define VKR_SERVE_SETTLE_SECONDS 0.3
#define VKR_SERVE_SEND_TIMEOUT_MS 2000u

// =============================================================================
// State
// =============================================================================

typedef struct VkrServeArguments {
  char *items[VKR_SERVE_MAX_ARGUMENTS + 2u];
  int count;
} VkrServeArguments;

typedef struct VkrServeClient {
  /* VKR_LOCAL_SOCKET_INVALID when the slot is free. */
  VkrLocalSocket socket;
  uint32_t generation;
  VkrBakeryBuffer inbox;
} VkrServeClient;

typedef struct VkrServeJob {
  uint32_t client;
  uint32_t client_generation;
  uint64_t request; /* Zero for a watch rebuild. */
  uint32_t watch;   /* Zero for a client run. */
  bool8_t background;
  VkrServeArguments arguments;
} VkrServeJob;

typedef struct VkrServeWatch {
  uint32_t id; /* Zero when the slot is free. */
  uint32_t client;
  uint32_t client_generation;
  char paths[VKR_SERVE_MAX_WATCH_PATHS][VKR_BAKERY_PATH_CAPACITY];
  uint32_t path_count;
  VkrServeArguments arguments; /* count zero: report changes only. */
  bool8_t pending;
  float64_t due;
  char changed[VKR_SERVE_MAX_CHANGED][VKR_BAKERY_PATH_CAPACITY];
  uint32_t changed_count;
} VkrServeWatch;

#if defined(_WIN32)
/* Directories one Windows watcher thread waits on, beside its stop event. */
#define VKR_SERVE_WATCH_DIRECTORIES (MAXIMUM_WAIT_OBJECTS - 1u)

/* One directory the Windows watcher reads changes from. */
typedef struct VkrServeDirectory {
  HANDLE handle;
  OVERLAPPED overlapped;
  /* A read is outstanding, so closing must cancel and wait for it. */
  bool8_t armed;
  char path[VKR_BAKERY_PATH_CAPACITY];
  /* 64 KiB, the largest buffer a network share accepts; DWORD-aligned. */
  DWORD buffer[16384];
} VkrServeDirectory;
#endif

typedef struct VkrServe {
  VkrAllocator allocator;
  char root[VKR_BAKERY_PATH_CAPACITY];
  char socket_path[VKR_BAKERY_PATH_CAPACITY];
  char program[VKR_BAKERY_PATH_CAPACITY];
  VkrLocalSocket listener;
  /* The loop polls wake[0]; other threads send a byte to wake[1]. */
  VkrLocalSocket wake[2];
  VkrServeClient clients[VKR_SERVE_MAX_CLIENTS];
  uint32_t next_generation;
  uint32_t next_watch;
  VkrServeWatch watches[VKR_SERVE_MAX_WATCHES];
  bool8_t watches_changed;

  /* Shared with the builder thread under `mutex`. */
  VkrMutex mutex;
  VkrCondVar ready;
  VkrServeJob queue[VKR_SERVE_MAX_JOBS];
  uint32_t queue_count;
  VkrServeJob running;
  bool8_t has_running;
  bool8_t stopping;
  /* Serializes socket writes from the loop and the builder. */
  VkrMutex write_mutex;

  /* Filled by the watcher thread under `changes_mutex`. */
  VkrMutex changes_mutex;
  char changes[256][VKR_BAKERY_PATH_CAPACITY];
  uint32_t change_count;
  bool8_t change_overflow;
  /* Paths the daemon's own runs just published; the watcher ignores their
     change events until `published_until`. */
  char published[128][VKR_BAKERY_PATH_CAPACITY];
  uint32_t published_count;
  float64_t published_until;
#if defined(__APPLE__)
  FSEventStreamRef stream;
  dispatch_queue_t stream_queue;
#elif defined(_WIN32)
  VkrServeDirectory *directories;
  uint32_t directory_count;
  HANDLE watcher;
  HANDLE watcher_stop;
#endif
} VkrServe;

vkr_internal volatile sig_atomic_t vkr_serve_signalled = 0;

vkr_internal void vkr_serve_signal(int signal_number) {
  (void)signal_number;
  vkr_serve_signalled = 1;
  vkr_atomic_bool_store(&vkr_bakery_cancel_requested, true_v,
                        VKR_MEMORY_ORDER_RELAXED);
}

vkr_internal void vkr_serve_wake(VkrServe *serve) {
  const char byte = 1;
  uint64_t sent = 0u;
  (void)vkr_local_socket_send(serve->wake[1], &byte, 1u, &sent);
}

/* First occurrence of `needle` in `length` bytes of `text`, or NULL. */
vkr_internal const char *vkr_serve_find(const char *text, uint64_t length,
                                        const char *needle) {
  const uint64_t needle_length = strlen(needle);
  for (uint64_t i = 0u; needle_length <= length && i <= length - needle_length;
       ++i) {
    if (MemCompare(text + i, needle, needle_length) == 0) {
      return text + i;
    }
  }
  return NULL;
}

// =============================================================================
// Socket paths
// =============================================================================

/* Local socket paths are limited to about 100 bytes, so the default lives in
   the per-user temporary directory, named by the root's digest. */
vkr_internal bool8_t vkr_serve_default_socket(const char *root, char *out,
                                              uint32_t capacity) {
  char digest[VKR_BAKERY_SHA256_HEX];
  vkr_bakery_hash_bytes(root, strlen(root), digest);
  char directory[VKR_BAKERY_PATH_CAPACITY];
  if (!vkr_platform_user_directory(VKR_PLATFORM_USER_TEMP, directory,
                                   sizeof(directory))) {
    return false_v;
  }
  const int written =
      snprintf(out, capacity, "%s/vkr-bakery-%.16s.sock", directory, digest);
  return written > 0 && (uint32_t)written < capacity &&
         vkr_local_socket_path_fits(out);
}

vkr_internal bool8_t vkr_serve_socket_path(const VkrBakeryCli *cli, char *out,
                                           uint32_t capacity) {
  if (cli->socket) {
    if (!vkr_local_socket_path_fits(cli->socket)) {
      fprintf(stderr, "error VKR-CLI-0001: --socket path is too long for a "
                      "local socket\n");
      return false_v;
    }
    (void)snprintf(out, capacity, "%s", cli->socket);
    return true_v;
  }
  if (!vkr_serve_default_socket(cli->config.root, out, capacity)) {
    fprintf(stderr, "error VKR-CLI-0001: the default socket path is too "
                    "long; pass --socket\n");
    return false_v;
  }
  return true_v;
}

/* Binds the socket; a stale socket file with no listener is replaced, while
   a live daemon for the same path makes this one exit. */
vkr_internal bool8_t vkr_serve_listen(VkrServe *serve) {
  const VkrLocalSocketListenStatus status =
      vkr_local_socket_listen(serve->socket_path, 8, &serve->listener);
  if (status == VKR_LOCAL_SOCKET_LISTEN_IN_USE) {
    fprintf(stderr, "error VKR-CLI-0004: a daemon already serves %s\n",
            serve->socket_path);
    return false_v;
  }
  if (status != VKR_LOCAL_SOCKET_LISTEN_OK) {
    char error[256];
    vkr_local_socket_error_text(error, sizeof(error));
    fprintf(stderr, "error VKR-CLI-0004: cannot listen on %s: %s\n",
            serve->socket_path, error);
    return false_v;
  }
  return true_v;
}

// =============================================================================
// Client writes
// =============================================================================

/* Sends one line to a client still holding `generation`; a client that
   stopped reading loses the line, not the daemon. */
vkr_internal void vkr_serve_send(VkrServe *serve, uint32_t client,
                                 uint32_t generation, const char *line,
                                 uint64_t length) {
  vkr_mutex_lock(serve->write_mutex);
  if (client < VKR_SERVE_MAX_CLIENTS &&
      serve->clients[client].socket != VKR_LOCAL_SOCKET_INVALID &&
      serve->clients[client].generation == generation) {
    (void)vkr_local_socket_send_all(serve->clients[client].socket, line,
                                    length);
  }
  vkr_mutex_unlock(serve->write_mutex);
}

vkr_internal void vkr_serve_sendf(VkrServe *serve, uint32_t client,
                                  uint32_t generation, const char *format,
                                  ...) {
  char line[1024];
  va_list arguments;
  va_start(arguments, format);
  const int length = vsnprintf(line, sizeof(line) - 1u, format, arguments);
  va_end(arguments);
  if (length <= 0 || (uint64_t)length >= sizeof(line) - 1u) {
    return;
  }
  line[length] = '\n';
  vkr_serve_send(serve, client, generation, line, (uint64_t)length + 1u);
}

/* The form the watcher reports `path` in: resolved through symbolic links on
   POSIX (FSEvents reports /private/tmp for /tmp) and absolute with '/'
   separators on Windows, as vkr_bake_resolve does. */
vkr_internal void vkr_serve_resolve(const char *path, char *out) {
#if defined(_WIN32)
  if (!vkr_bakery_path_absolute(path, out, VKR_BAKERY_PATH_CAPACITY)) {
    (void)snprintf(out, VKR_BAKERY_PATH_CAPACITY, "%s", path);
  }
#else
  if (!realpath(path, out)) {
    (void)snprintf(out, VKR_BAKERY_PATH_CAPACITY, "%s", path);
  }
#endif
}

/* Records the output paths of a `done` event so the watcher can tell the
   daemon's own publications from edits. */
vkr_internal void vkr_serve_note_outputs(VkrServe *serve, const char *line,
                                         uint64_t length) {
  if (!vkr_serve_find(line, length, "\"ev\":\"done\"")) {
    return;
  }
  Arena *arena = arena_create(MB(1), KB(64));
  if (!arena) {
    return;
  }
  const VkrBakeryJson *event =
      vkr_bakery_json_parse(arena, (const uint8_t *)line, length, 8u, NULL);
  const VkrBakeryJson *outputs = vkr_bakery_json_get(event, "outputs");
  vkr_mutex_lock(serve->changes_mutex);
  for (const VkrBakeryJson *output = outputs ? outputs->first : NULL; output;
       output = output->next) {
    if (output->type != VKR_BAKERY_JSON_STRING ||
        serve->published_count == ArrayCount(serve->published)) {
      continue;
    }
    vkr_serve_resolve((const char *)output->string.str,
                      serve->published[serve->published_count++]);
  }
  vkr_mutex_unlock(serve->changes_mutex);
  arena_destroy(arena);
}

/* Event sink of the running command: tags each line with its request. */
vkr_internal void vkr_serve_event_sink(void *context, const char *line,
                                       uint64_t length) {
  VkrServe *serve = (VkrServe *)context;
  const VkrServeJob *job = &serve->running;
  if (length < 2u || line[0] != '{') {
    return;
  }
  vkr_serve_note_outputs(serve, line, length);
  char tag[64];
  const int tag_length =
      job->watch ? snprintf(tag, sizeof(tag), "{\"watch\":%u,", job->watch)
                 : snprintf(tag, sizeof(tag), "{\"req\":%llu,",
                            (unsigned long long)job->request);
  VkrBakeryBuffer tagged = {0};
  vkr_bakery_buffer_append(&tagged, tag, (uint64_t)tag_length);
  vkr_bakery_buffer_append(&tagged, line + 1, length - 1u);
  if (!tagged.failed) {
    vkr_serve_send(serve, job->client, job->client_generation,
                   (const char *)tagged.data, tagged.length);
  }
  vkr_bakery_buffer_free(&tagged);
}

// =============================================================================
// Jobs
// =============================================================================

vkr_internal bool8_t vkr_serve_command_allowed(const char *command) {
  static const char *const allowed[] = {"cook",      "build",  "shaders",
                                        "materials", "status", "inspect",
                                        "explain",   "gc"};
  for (uint32_t i = 0u; command && i < ArrayCount(allowed); ++i) {
    if (strcmp(command, allowed[i]) == 0) {
      return true_v;
    }
  }
  return false_v;
}

vkr_internal void vkr_serve_arguments_free(VkrServeArguments *arguments) {
  for (int i = 1; i < arguments->count; ++i) {
    free(arguments->items[i]);
  }
  MemZero(arguments, sizeof(*arguments));
}

/* argv[0] is this program; `values` holds the subcommand and its options. */
vkr_internal bool8_t vkr_serve_arguments_copy(VkrServe *serve,
                                              const VkrBakeryJson *values,
                                              VkrServeArguments *out,
                                              const char **out_error) {
  MemZero(out, sizeof(*out));
  if (!values || values->type != VKR_BAKERY_JSON_ARRAY || !values->count ||
      values->count > VKR_SERVE_MAX_ARGUMENTS) {
    *out_error = "argv must be a non-empty array of strings";
    return false_v;
  }
  out->items[out->count++] = serve->program;
  for (const VkrBakeryJson *value = values->first; value; value = value->next) {
    if (value->type != VKR_BAKERY_JSON_STRING) {
      vkr_serve_arguments_free(out);
      *out_error = "argv must be a non-empty array of strings";
      return false_v;
    }
    char *copy = (char *)malloc(value->string.length + 1u);
    if (!copy) {
      vkr_serve_arguments_free(out);
      *out_error = "out of memory";
      return false_v;
    }
    MemCopy(copy, value->string.str, value->string.length);
    copy[value->string.length] = 0;
    out->items[out->count++] = copy;
  }
  if (!vkr_serve_command_allowed(out->items[1])) {
    vkr_serve_arguments_free(out);
    *out_error = "the daemon runs cook, build, shaders, materials, status, "
                 "inspect, explain and gc";
    return false_v;
  }
  return true_v;
}

vkr_internal bool8_t vkr_serve_arguments_clone(const VkrServeArguments *source,
                                               VkrServeArguments *out) {
  MemZero(out, sizeof(*out));
  out->items[out->count++] = source->items[0];
  for (int i = 1; i < source->count; ++i) {
    const uint64_t length = strlen(source->items[i]) + 1u;
    char *copy = (char *)malloc(length);
    if (!copy) {
      vkr_serve_arguments_free(out);
      return false_v;
    }
    MemCopy(copy, source->items[i], length);
    out->items[out->count++] = copy;
  }
  return true_v;
}

/* Queues a job; interactive jobs go before every background job. Called
   with `mutex` held. */
vkr_internal bool8_t vkr_serve_enqueue(VkrServe *serve, VkrServeJob *job) {
  if (serve->queue_count == VKR_SERVE_MAX_JOBS) {
    return false_v;
  }
  uint32_t slot = serve->queue_count;
  if (!job->background) {
    slot = 0u;
    while (slot < serve->queue_count && !serve->queue[slot].background) {
      slot += 1u;
    }
    MemCopy(&serve->queue[slot + 1u], &serve->queue[slot],
            (serve->queue_count - slot) * sizeof(VkrServeJob));
  }
  serve->queue[slot] = *job;
  serve->queue_count += 1u;
  vkr_cond_signal(serve->ready);
  return true_v;
}

vkr_internal void *vkr_serve_builder(void *context) {
  VkrServe *serve = (VkrServe *)context;
  for (;;) {
    vkr_mutex_lock(serve->mutex);
    while (!serve->stopping && serve->queue_count == 0u) {
      vkr_cond_wait(serve->ready, serve->mutex);
    }
    if (serve->stopping) {
      vkr_mutex_unlock(serve->mutex);
      return NULL;
    }
    serve->running = serve->queue[0];
    serve->queue_count -= 1u;
    MemCopy(&serve->queue[0], &serve->queue[1],
            serve->queue_count * sizeof(VkrServeJob));
    serve->has_running = true_v;
    vkr_mutex_unlock(serve->mutex);

    /* Outputs written while the command runs are its own. */
    vkr_mutex_lock(serve->changes_mutex);
    serve->published_until = 1e300;
    vkr_mutex_unlock(serve->changes_mutex);
    VkrServeJob *job = &serve->running;
    const int code =
        vkr_bakery_run_command(job->arguments.count, job->arguments.items,
                               vkr_serve_event_sink, serve);
    if (job->watch) {
      vkr_serve_sendf(serve, job->client, job->client_generation,
                      "{\"v\":1,\"ev\":\"reply\",\"watch\":%u,\"exit\":%d}",
                      job->watch, code);
    } else {
      vkr_serve_sendf(serve, job->client, job->client_generation,
                      "{\"v\":1,\"ev\":\"reply\",\"req\":%llu,\"exit\":%d}",
                      (unsigned long long)job->request, code);
    }

    /* Publication events arrive shortly after the files change. */
    vkr_mutex_lock(serve->changes_mutex);
    serve->published_until = vkr_bakery_monotonic_seconds() + 2.0;
    vkr_mutex_unlock(serve->changes_mutex);

    vkr_mutex_lock(serve->mutex);
    vkr_serve_arguments_free(&job->arguments);
    serve->has_running = false_v;
    /* A cancel request or a stopping daemon ends only the job it targeted. */
    if (!vkr_serve_signalled) {
      vkr_atomic_bool_store(&vkr_bakery_cancel_requested, false_v,
                            VKR_MEMORY_ORDER_RELAXED);
    }
    vkr_mutex_unlock(serve->mutex);
    vkr_serve_wake(serve);
  }
}

// =============================================================================
// Watches
// =============================================================================

vkr_internal void vkr_serve_note_change(VkrServe *serve, const char *path) {
  vkr_mutex_lock(serve->changes_mutex);
  if (serve->change_count < ArrayCount(serve->changes)) {
    (void)snprintf(serve->changes[serve->change_count++],
                   VKR_BAKERY_PATH_CAPACITY, "%s", path);
  } else {
    serve->change_overflow = true_v;
  }
  vkr_mutex_unlock(serve->changes_mutex);
  vkr_serve_wake(serve);
}

vkr_internal bool8_t vkr_serve_separator(char c) {
#if defined(_WIN32)
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

/* The first `length` bytes of `path` and `prefix` name the same path. Windows
   file systems ignore case and accept either separator. */
vkr_internal bool8_t vkr_serve_path_prefix(const char *path, const char *prefix,
                                           uint64_t length) {
#if defined(_WIN32)
  for (uint64_t i = 0u; i < length; ++i) {
    const char a = path[i];
    const char b = prefix[i];
    if (vkr_serve_separator(a) && vkr_serve_separator(b)) {
      continue;
    }
    const char folded_a = a >= 'A' && a <= 'Z' ? (char)(a + 32) : a;
    const char folded_b = b >= 'A' && b <= 'Z' ? (char)(b + 32) : b;
    if (!a || folded_a != folded_b) {
      return false_v;
    }
  }
  return true_v;
#else
  return strncmp(path, prefix, length) == 0;
#endif
}

vkr_internal bool8_t vkr_serve_path_under(const char *path, const char *root) {
  const uint64_t length = strlen(root);
  return vkr_serve_path_prefix(path, root, length) &&
         (path[length] == 0 || vkr_serve_separator(path[length]));
}

vkr_internal bool8_t vkr_serve_path_equal(const char *a, const char *b) {
  const uint64_t length = strlen(b);
  return strlen(a) == length && vkr_serve_path_prefix(a, b, length);
}

#if defined(__APPLE__)

vkr_internal void vkr_serve_fsevents(ConstFSEventStreamRef stream,
                                     void *context, size_t count, void *paths,
                                     const FSEventStreamEventFlags flags[],
                                     const FSEventStreamEventId ids[]) {
  (void)stream;
  (void)flags;
  (void)ids;
  VkrServe *serve = (VkrServe *)context;
  char **names = (char **)paths;
  for (size_t i = 0u; i < count; ++i) {
    vkr_serve_note_change(serve, names[i]);
  }
}

vkr_internal void vkr_serve_stop_stream(VkrServe *serve) {
  if (serve->stream) {
    FSEventStreamStop(serve->stream);
    FSEventStreamInvalidate(serve->stream);
    FSEventStreamRelease(serve->stream);
    serve->stream = NULL;
  }
}

/* One FSEvents stream covers every watched path; it is rebuilt whenever the
   watch set changes. */
vkr_internal bool8_t vkr_serve_restart_stream(VkrServe *serve) {
  vkr_serve_stop_stream(serve);
  CFMutableArrayRef paths =
      CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
  if (!paths) {
    return false_v;
  }
  for (uint32_t w = 0u; w < VKR_SERVE_MAX_WATCHES; ++w) {
    const VkrServeWatch *watch = &serve->watches[w];
    for (uint32_t p = 0u; watch->id && p < watch->path_count; ++p) {
      CFStringRef path = CFStringCreateWithCString(NULL, watch->paths[p],
                                                   kCFStringEncodingUTF8);
      if (path) {
        CFArrayAppendValue(paths, path);
        CFRelease(path);
      }
    }
  }
  bool8_t ok = true_v;
  if (CFArrayGetCount(paths) > 0) {
    FSEventStreamContext context = {.info = serve};
    serve->stream = FSEventStreamCreate(
        NULL, vkr_serve_fsevents, &context, paths,
        kFSEventStreamEventIdSinceNow, 0.05,
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
    ok = serve->stream != NULL;
    if (ok) {
      FSEventStreamSetDispatchQueue(serve->stream, serve->stream_queue);
      ok = FSEventStreamStart(serve->stream) ? true_v : false_v;
    }
  }
  CFRelease(paths);
  return ok;
}

#elif defined(_WIN32)

vkr_internal bool8_t vkr_serve_directory_read(VkrServeDirectory *directory) {
  (void)ResetEvent(directory->overlapped.hEvent);
  directory->armed =
      ReadDirectoryChangesW(
          directory->handle, directory->buffer, sizeof(directory->buffer), TRUE,
          FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
              FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
              FILE_NOTIFY_CHANGE_CREATION,
          NULL, &directory->overlapped, NULL)
          ? true_v
          : false_v;
  return directory->armed;
}

/* Reports each changed path of a completed read; an empty read means the
   buffer overflowed and individual paths are lost. */
vkr_internal void vkr_serve_directory_changes(VkrServe *serve,
                                              VkrServeDirectory *directory,
                                              DWORD bytes) {
  if (!bytes) {
    vkr_mutex_lock(serve->changes_mutex);
    serve->change_overflow = true_v;
    vkr_mutex_unlock(serve->changes_mutex);
    vkr_serve_wake(serve);
    return;
  }
  const uint8_t *cursor = (const uint8_t *)directory->buffer;
  for (;;) {
    const FILE_NOTIFY_INFORMATION *info =
        (const FILE_NOTIFY_INFORMATION *)cursor;
    char name[VKR_BAKERY_PATH_CAPACITY];
    char path[VKR_BAKERY_PATH_CAPACITY];
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, info->FileName, (int)(info->FileNameLength / sizeof(WCHAR)),
        name, (int)sizeof(name) - 1, NULL, NULL);
    if (length > 0) {
      name[length] = 0;
      if (vkr_bakery_path_join(path, sizeof(path), directory->path, name)) {
        vkr_bakery_path_portable(path);
        vkr_serve_note_change(serve, path);
      }
    }
    if (!info->NextEntryOffset) {
      break;
    }
    cursor += info->NextEntryOffset;
  }
}

/* Waits on every directory's read and the stop event; a directory whose read
   cannot be rearmed leaves the wait set. */
vkr_internal DWORD WINAPI vkr_serve_watcher(LPVOID context) {
  VkrServe *serve = (VkrServe *)context;
  HANDLE handles[VKR_SERVE_WATCH_DIRECTORIES + 1u];
  uint32_t indices[VKR_SERVE_WATCH_DIRECTORIES + 1u];
  uint32_t count = 0u;
  handles[count++] = serve->watcher_stop;
  for (uint32_t i = 0u; i < serve->directory_count; ++i) {
    if (serve->directories[i].armed) {
      indices[count] = i;
      handles[count++] = serve->directories[i].overlapped.hEvent;
    }
  }
  for (;;) {
    const DWORD signalled =
        WaitForMultipleObjects((DWORD)count, handles, FALSE, INFINITE);
    if (signalled <= WAIT_OBJECT_0 || signalled >= WAIT_OBJECT_0 + count) {
      return 0u;
    }
    const uint32_t slot = (uint32_t)(signalled - WAIT_OBJECT_0);
    VkrServeDirectory *directory = &serve->directories[indices[slot]];
    DWORD bytes = 0u;
    if (!GetOverlappedResult(directory->handle, &directory->overlapped, &bytes,
                             FALSE)) {
      bytes = 0u;
    }
    directory->armed = false_v;
    vkr_serve_directory_changes(serve, directory, bytes);
    if (!vkr_serve_directory_read(directory)) {
      count -= 1u;
      handles[slot] = handles[count];
      indices[slot] = indices[count];
    }
  }
}

vkr_internal void vkr_serve_stop_stream(VkrServe *serve) {
  if (serve->watcher) {
    (void)SetEvent(serve->watcher_stop);
    (void)WaitForSingleObject(serve->watcher, INFINITE);
    (void)CloseHandle(serve->watcher);
    serve->watcher = NULL;
  }
  for (uint32_t i = 0u; i < serve->directory_count; ++i) {
    VkrServeDirectory *directory = &serve->directories[i];
    /* The kernel writes the buffer until a cancelled read completes. */
    if (directory->armed) {
      DWORD bytes = 0u;
      (void)CancelIoEx(directory->handle, &directory->overlapped);
      (void)GetOverlappedResult(directory->handle, &directory->overlapped,
                                &bytes, TRUE);
    }
    (void)CloseHandle(directory->handle);
    (void)CloseHandle(directory->overlapped.hEvent);
  }
  free(serve->directories);
  serve->directories = NULL;
  serve->directory_count = 0u;
  if (serve->watcher_stop) {
    (void)CloseHandle(serve->watcher_stop);
    serve->watcher_stop = NULL;
  }
}

/* Opens `path` for change reads; false when it cannot be watched. */
vkr_internal bool8_t vkr_serve_directory_open(VkrServeDirectory *directory,
                                              const char *path) {
  wchar_t wide[VKR_BAKERY_PATH_CAPACITY];
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide,
                           (int)ArrayCount(wide))) {
    return false_v;
  }
  for (wchar_t *c = wide; *c; ++c) {
    if (*c == L'/') {
      *c = L'\\';
    }
  }
  directory->handle = CreateFileW(
      wide, FILE_LIST_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
  if (directory->handle == INVALID_HANDLE_VALUE) {
    return false_v;
  }
  directory->overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  (void)snprintf(directory->path, sizeof(directory->path), "%s", path);
  if (!directory->overlapped.hEvent || !vkr_serve_directory_read(directory)) {
    (void)CloseHandle(directory->handle);
    if (directory->overlapped.hEvent) {
      (void)CloseHandle(directory->overlapped.hEvent);
    }
    MemZero(directory, sizeof(*directory));
    return false_v;
  }
  return true_v;
}

/* Each watched directory, or the parent of each watched file, is read with
   its subtree; a directory under one already read is skipped. The watcher
   thread is rebuilt whenever the watch set changes. */
vkr_internal bool8_t vkr_serve_restart_stream(VkrServe *serve) {
  vkr_serve_stop_stream(serve);
  char(*chosen)[VKR_BAKERY_PATH_CAPACITY] =
      malloc(VKR_SERVE_WATCH_DIRECTORIES * VKR_BAKERY_PATH_CAPACITY);
  if (!chosen) {
    return false_v;
  }
  uint32_t chosen_count = 0u;
  bool8_t complete = true_v;
  for (uint32_t w = 0u; w < VKR_SERVE_MAX_WATCHES; ++w) {
    const VkrServeWatch *watch = &serve->watches[w];
    for (uint32_t p = 0u; watch->id && p < watch->path_count; ++p) {
      char directory[VKR_BAKERY_PATH_CAPACITY];
      VkrBakeryStat info = {0};
      if (vkr_bakery_stat(watch->paths[p], &info) && info.exists &&
          info.is_directory) {
        (void)snprintf(directory, sizeof(directory), "%s", watch->paths[p]);
      } else {
        vkr_bakery_path_parent(directory, sizeof(directory), watch->paths[p]);
      }
      bool8_t covered = !directory[0];
      for (uint32_t c = 0u; c < chosen_count && !covered; ++c) {
        covered = vkr_serve_path_under(directory, chosen[c]);
      }
      if (covered) {
        continue;
      }
      if (chosen_count == VKR_SERVE_WATCH_DIRECTORIES) {
        complete = false_v;
        continue;
      }
      MemCopy(chosen[chosen_count++], directory, strlen(directory) + 1u);
    }
  }
  if (!chosen_count) {
    free(chosen);
    return complete;
  }

  serve->watcher_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
  serve->directories =
      (VkrServeDirectory *)calloc(chosen_count, sizeof(VkrServeDirectory));
  if (!serve->watcher_stop || !serve->directories) {
    free(chosen);
    vkr_serve_stop_stream(serve);
    return false_v;
  }
  for (uint32_t c = 0u; c < chosen_count; ++c) {
    /* A path that does not exist yet has nothing to read. */
    if (vkr_serve_directory_open(&serve->directories[serve->directory_count],
                                 chosen[c])) {
      serve->directory_count += 1u;
    }
  }
  free(chosen);
  if (!serve->directory_count) {
    vkr_serve_stop_stream(serve);
    return false_v;
  }
  serve->watcher = CreateThread(NULL, 0u, vkr_serve_watcher, serve, 0u, NULL);
  if (!serve->watcher) {
    vkr_serve_stop_stream(serve);
    return false_v;
  }
  return complete;
}

#else

vkr_internal void vkr_serve_stop_stream(VkrServe *serve) { (void)serve; }

/* Other POSIX hosts have no watcher yet; watches report only explicit
   requests. */
vkr_internal bool8_t vkr_serve_restart_stream(VkrServe *serve) {
  (void)serve;
  return false_v;
}

#endif

/* Matches drained changes against watches; returns the earliest due time. */
vkr_internal void vkr_serve_match_changes(VkrServe *serve, float64_t now) {
  static char changes[256][VKR_BAKERY_PATH_CAPACITY];
  vkr_mutex_lock(serve->changes_mutex);
  if (serve->published_until > now + 3600.0) {
    /* A command is running: its outputs are known only when it ends, so its
       changes wait until then. */
    vkr_mutex_unlock(serve->changes_mutex);
    return;
  }
  const bool8_t overflow = serve->change_overflow;
  const bool8_t recent = now <= serve->published_until;
  uint32_t count = 0u;
  for (uint32_t c = 0u; c < serve->change_count; ++c) {
    const char *path = serve->changes[c];
    const char *name = strrchr(path, '/');
    const uint64_t length = strlen(path);
    /* Atomic writers stage dot-prefixed .tmp siblings. */
    bool8_t ignored = name && name[1] == '.' && length > 4u &&
                      strcmp(path + length - 4u, ".tmp") == 0;
    for (uint32_t p = 0u; recent && !ignored && p < serve->published_count;
         ++p) {
      ignored = vkr_serve_path_equal(path, serve->published[p]);
    }
    if (!ignored) {
      MemCopy(changes[count++], path, length + 1u);
    }
  }
  if (!recent) {
    serve->published_count = 0u;
  }
  serve->change_count = 0u;
  serve->change_overflow = false_v;
  vkr_mutex_unlock(serve->changes_mutex);
  for (uint32_t w = 0u; w < VKR_SERVE_MAX_WATCHES; ++w) {
    VkrServeWatch *watch = &serve->watches[w];
    if (!watch->id) {
      continue;
    }
    for (uint32_t c = 0u; c < count; ++c) {
      bool8_t matched = false_v;
      for (uint32_t p = 0u; p < watch->path_count && !matched; ++p) {
        matched = vkr_serve_path_under(changes[c], watch->paths[p]);
      }
      if (!matched) {
        continue;
      }
      bool8_t listed = false_v;
      for (uint32_t i = 0u; i < watch->changed_count && !listed; ++i) {
        listed = vkr_serve_path_equal(watch->changed[i], changes[c]);
      }
      if (!listed && watch->changed_count < VKR_SERVE_MAX_CHANGED) {
        (void)snprintf(watch->changed[watch->changed_count++],
                       VKR_BAKERY_PATH_CAPACITY, "%s", changes[c]);
      }
      watch->pending = true_v;
      watch->due = now + VKR_SERVE_SETTLE_SECONDS;
    }
    if (overflow && watch->path_count) {
      /* Lost individual paths still mean something changed. */
      watch->pending = true_v;
      watch->due = now + VKR_SERVE_SETTLE_SECONDS;
    }
  }
}

vkr_internal void vkr_serve_fire_watch(VkrServe *serve, VkrServeWatch *watch) {
  VkrBakeryBuffer line = {0};
  vkr_bakery_buffer_appendf(&line,
                            "{\"v\":1,\"ev\":\"changed\",\"watch\":%u,"
                            "\"paths\":[",
                            watch->id);
  for (uint32_t i = 0u; i < watch->changed_count; ++i) {
    if (i) {
      vkr_bakery_buffer_append(&line, ",", 1u);
    }
    vkr_bakery_json_write_string(
        &line, (String8){.str = (uint8_t *)watch->changed[i],
                         .length = strlen(watch->changed[i])});
  }
  vkr_bakery_buffer_append(&line, "]}\n", 3u);
  if (!line.failed) {
    vkr_serve_send(serve, watch->client, watch->client_generation,
                   (const char *)line.data, line.length);
  }
  vkr_bakery_buffer_free(&line);
  watch->pending = false_v;
  watch->changed_count = 0u;
  if (!watch->arguments.count) {
    return;
  }
  vkr_mutex_lock(serve->mutex);
  /* A rebuild already queued for this watch absorbs the new change. */
  bool8_t queued = false_v;
  for (uint32_t i = 0u; i < serve->queue_count && !queued; ++i) {
    queued = serve->queue[i].watch == watch->id;
  }
  if (!queued) {
    VkrServeJob job = {.client = watch->client,
                       .client_generation = watch->client_generation,
                       .watch = watch->id,
                       .background = true_v};
    if (vkr_serve_arguments_clone(&watch->arguments, &job.arguments) &&
        !vkr_serve_enqueue(serve, &job)) {
      vkr_serve_arguments_free(&job.arguments);
    }
  }
  vkr_mutex_unlock(serve->mutex);
}

vkr_internal void vkr_serve_drop_watch(VkrServe *serve, VkrServeWatch *watch) {
  vkr_serve_arguments_free(&watch->arguments);
  MemZero(watch, sizeof(*watch));
  serve->watches_changed = true_v;
}

// =============================================================================
// Requests
// =============================================================================

vkr_internal void vkr_serve_reply_error(VkrServe *serve, uint32_t client,
                                        uint64_t request, const char *error) {
  VkrBakeryBuffer line = {0};
  vkr_bakery_buffer_appendf(
      &line,
      "{\"v\":1,\"ev\":\"reply\",\"req\":%llu,\"exit\":%d,"
      "\"error\":",
      (unsigned long long)request, VKR_BAKERY_EXIT_USAGE);
  vkr_bakery_json_write_string(
      &line, (String8){.str = (uint8_t *)error, .length = strlen(error)});
  vkr_bakery_buffer_append(&line, "}\n", 2u);
  if (!line.failed) {
    vkr_serve_send(serve, client, serve->clients[client].generation,
                   (const char *)line.data, line.length);
  }
  vkr_bakery_buffer_free(&line);
}

vkr_internal void vkr_serve_request_watch(VkrServe *serve, uint32_t client,
                                          uint64_t request,
                                          const VkrBakeryJson *document) {
  const VkrBakeryJson *paths = vkr_bakery_json_get(document, "paths");
  if (!paths || paths->type != VKR_BAKERY_JSON_ARRAY || !paths->count ||
      paths->count > VKR_SERVE_MAX_WATCH_PATHS) {
    vkr_serve_reply_error(serve, client, request,
                          "paths must list 1 to 16 files or directories");
    return;
  }
  VkrServeWatch *watch = NULL;
  for (uint32_t w = 0u; w < VKR_SERVE_MAX_WATCHES && !watch; ++w) {
    watch = serve->watches[w].id ? NULL : &serve->watches[w];
  }
  if (!watch) {
    vkr_serve_reply_error(serve, client, request, "too many watches");
    return;
  }
  VkrServeWatch prepared = {
      .client = client, .client_generation = serve->clients[client].generation};
  for (const VkrBakeryJson *path = paths->first; path; path = path->next) {
    char joined[VKR_BAKERY_PATH_CAPACITY];
    char absolute[VKR_BAKERY_PATH_CAPACITY];
    if (path->type != VKR_BAKERY_JSON_STRING ||
        !vkr_bakery_path_join(
            joined, sizeof(joined),
            vkr_bakery_path_is_absolute((const char *)path->string.str)
                ? ""
                : serve->root,
            (const char *)path->string.str) ||
        !vkr_bakery_path_absolute(joined, absolute, sizeof(absolute))) {
      vkr_serve_reply_error(serve, client, request, "invalid watch path");
      return;
    }
    vkr_serve_resolve(absolute, prepared.paths[prepared.path_count++]);
  }
  const VkrBakeryJson *argv = vkr_bakery_json_get(document, "argv");
  const char *error = NULL;
  if (argv &&
      !vkr_serve_arguments_copy(serve, argv, &prepared.arguments, &error)) {
    vkr_serve_reply_error(serve, client, request, error);
    return;
  }
  prepared.id = ++serve->next_watch;
  *watch = prepared;
  serve->watches_changed = true_v;
  vkr_serve_sendf(serve, client, watch->client_generation,
                  "{\"v\":1,\"ev\":\"reply\",\"req\":%llu,\"exit\":0,"
                  "\"watch\":%u}",
                  (unsigned long long)request, watch->id);
}

vkr_internal void vkr_serve_request_cancel(VkrServe *serve, uint32_t client,
                                           uint64_t request, uint64_t target) {
  const uint32_t generation = serve->clients[client].generation;
  bool8_t found = false_v;
  vkr_mutex_lock(serve->mutex);
  for (uint32_t i = 0u; i < serve->queue_count; ++i) {
    VkrServeJob *job = &serve->queue[i];
    if (job->client == client && job->client_generation == generation &&
        job->request == target) {
      vkr_serve_sendf(serve, client, generation,
                      "{\"v\":1,\"ev\":\"reply\",\"req\":%llu,\"exit\":%d}",
                      (unsigned long long)target, VKR_BAKERY_EXIT_CANCELLED);
      vkr_serve_arguments_free(&job->arguments);
      serve->queue_count -= 1u;
      MemCopy(&serve->queue[i], &serve->queue[i + 1u],
              (serve->queue_count - i) * sizeof(VkrServeJob));
      found = true_v;
      break;
    }
  }
  if (!found && serve->has_running && serve->running.client == client &&
      serve->running.client_generation == generation &&
      serve->running.request == target) {
    vkr_atomic_bool_store(&vkr_bakery_cancel_requested, true_v,
                          VKR_MEMORY_ORDER_RELAXED);
    found = true_v;
  }
  vkr_mutex_unlock(serve->mutex);
  vkr_serve_sendf(serve, client, generation,
                  "{\"v\":1,\"ev\":\"reply\",\"req\":%llu,\"exit\":%d}",
                  (unsigned long long)request, found ? 0 : 1);
}

vkr_internal void vkr_serve_request(VkrServe *serve, uint32_t client,
                                    const char *text, uint64_t length) {
  Arena *arena = arena_create(MB(4), KB(64));
  if (!arena) {
    return;
  }
  VkrBakeryJsonError error = {0};
  const VkrBakeryJson *document =
      vkr_bakery_json_parse(arena, (const uint8_t *)text, length, 16u, &error);
  int64_t version = 0;
  int64_t request = 0;
  const char *kind = document ? (const char *)vkr_bakery_json_cstr_value(
                                    arena, vkr_bakery_json_get(document, "req"))
                              : NULL;
  if (!document || !vkr_bakery_json_get_int(document, "v", &version) ||
      version != VKR_SERVE_PROTOCOL ||
      !vkr_bakery_json_get_int(document, "id", &request) || request <= 0 ||
      !kind) {
    vkr_serve_reply_error(serve, client, request > 0 ? (uint64_t)request : 0u,
                          "requests need v 1, a positive id and req");
    arena_destroy(arena);
    return;
  }
  const uint32_t generation = serve->clients[client].generation;
  if (strcmp(kind, "ping") == 0) {
    vkr_serve_sendf(serve, client, generation,
                    "{\"v\":1,\"ev\":\"reply\",\"req\":%lld,\"exit\":0,"
                    "\"protocol\":%u}",
                    (long long)request, VKR_SERVE_PROTOCOL);
  } else if (strcmp(kind, "run") == 0) {
    VkrServeJob job = {.client = client,
                       .client_generation = generation,
                       .request = (uint64_t)request};
    const char *failure = NULL;
    const char *priority = (const char *)vkr_bakery_json_cstr_value(
        arena, vkr_bakery_json_get(document, "priority"));
    job.background = priority && strcmp(priority, "background") == 0;
    if (!vkr_serve_arguments_copy(serve, vkr_bakery_json_get(document, "argv"),
                                  &job.arguments, &failure)) {
      vkr_serve_reply_error(serve, client, (uint64_t)request, failure);
    } else {
      vkr_mutex_lock(serve->mutex);
      const bool8_t queued = vkr_serve_enqueue(serve, &job);
      vkr_mutex_unlock(serve->mutex);
      if (!queued) {
        vkr_serve_arguments_free(&job.arguments);
        vkr_serve_reply_error(serve, client, (uint64_t)request,
                              "the request queue is full");
      }
    }
  } else if (strcmp(kind, "watch") == 0) {
    vkr_serve_request_watch(serve, client, (uint64_t)request, document);
  } else if (strcmp(kind, "unwatch") == 0) {
    int64_t id = 0;
    bool8_t found = false_v;
    (void)vkr_bakery_json_get_int(document, "watch", &id);
    for (uint32_t w = 0u; w < VKR_SERVE_MAX_WATCHES; ++w) {
      VkrServeWatch *watch = &serve->watches[w];
      if (watch->id && (int64_t)watch->id == id && watch->client == client) {
        vkr_serve_drop_watch(serve, watch);
        found = true_v;
      }
    }
    vkr_serve_sendf(serve, client, generation,
                    "{\"v\":1,\"ev\":\"reply\",\"req\":%lld,\"exit\":%d}",
                    (long long)request, found ? 0 : 1);
  } else if (strcmp(kind, "cancel") == 0) {
    int64_t target = 0;
    (void)vkr_bakery_json_get_int(document, "target", &target);
    vkr_serve_request_cancel(serve, client, (uint64_t)request,
                             (uint64_t)target);
  } else if (strcmp(kind, "shutdown") == 0) {
    vkr_serve_sendf(serve, client, generation,
                    "{\"v\":1,\"ev\":\"reply\",\"req\":%lld,\"exit\":0}",
                    (long long)request);
    vkr_serve_signalled = 1;
    vkr_atomic_bool_store(&vkr_bakery_cancel_requested, true_v,
                          VKR_MEMORY_ORDER_RELAXED);
  } else {
    vkr_serve_reply_error(serve, client, (uint64_t)request, "unknown request");
  }
  arena_destroy(arena);
}

// =============================================================================
// Connections
// =============================================================================

vkr_internal void vkr_serve_close_client(VkrServe *serve, uint32_t index) {
  VkrServeClient *client = &serve->clients[index];
  vkr_mutex_lock(serve->write_mutex);
  vkr_local_socket_close(client->socket);
  client->socket = VKR_LOCAL_SOCKET_INVALID;
  vkr_mutex_unlock(serve->write_mutex);
  vkr_bakery_buffer_free(&client->inbox);
  /* Watches die with their client; its queued runs are dropped. */
  for (uint32_t w = 0u; w < VKR_SERVE_MAX_WATCHES; ++w) {
    if (serve->watches[w].id && serve->watches[w].client == index &&
        serve->watches[w].client_generation == client->generation) {
      vkr_serve_drop_watch(serve, &serve->watches[w]);
    }
  }
  vkr_mutex_lock(serve->mutex);
  uint32_t kept = 0u;
  for (uint32_t i = 0u; i < serve->queue_count; ++i) {
    VkrServeJob *job = &serve->queue[i];
    if (job->client == index && job->client_generation == client->generation) {
      vkr_serve_arguments_free(&job->arguments);
    } else {
      serve->queue[kept++] = *job;
    }
  }
  serve->queue_count = kept;
  if (serve->has_running && serve->running.client == index &&
      serve->running.client_generation == client->generation) {
    vkr_atomic_bool_store(&vkr_bakery_cancel_requested, true_v,
                          VKR_MEMORY_ORDER_RELAXED);
  }
  vkr_mutex_unlock(serve->mutex);
}

vkr_internal void vkr_serve_accept(VkrServe *serve) {
  VkrLocalSocket accepted = VKR_LOCAL_SOCKET_INVALID;
  if (vkr_local_socket_accept(serve->listener, &accepted) !=
      VKR_LOCAL_SOCKET_OK) {
    return;
  }
  uint32_t slot = VKR_SERVE_MAX_CLIENTS;
  for (uint32_t i = 0u;
       i < VKR_SERVE_MAX_CLIENTS && slot == VKR_SERVE_MAX_CLIENTS; ++i) {
    slot = serve->clients[i].socket == VKR_LOCAL_SOCKET_INVALID ? i : slot;
  }
  if (slot == VKR_SERVE_MAX_CLIENTS) {
    vkr_local_socket_close(accepted);
    return;
  }
  (void)vkr_local_socket_set_send_timeout_ms(accepted,
                                             VKR_SERVE_SEND_TIMEOUT_MS);
  vkr_mutex_lock(serve->write_mutex);
  serve->clients[slot].socket = accepted;
  serve->clients[slot].generation = ++serve->next_generation;
  vkr_mutex_unlock(serve->write_mutex);
  MemZero(&serve->clients[slot].inbox, sizeof(serve->clients[slot].inbox));
}

vkr_internal void vkr_serve_read(VkrServe *serve, uint32_t index) {
  VkrServeClient *client = &serve->clients[index];
  char chunk[4096];
  uint64_t received = 0u;
  const VkrLocalSocketStatus status =
      vkr_local_socket_recv(client->socket, chunk, sizeof(chunk), &received);
  if (status == VKR_LOCAL_SOCKET_WOULD_BLOCK) {
    return;
  }
  if (status != VKR_LOCAL_SOCKET_OK) {
    vkr_serve_close_client(serve, index);
    return;
  }
  vkr_bakery_buffer_append(&client->inbox, chunk, received);
  if (client->inbox.failed ||
      client->inbox.length > VKR_SERVE_MAX_REQUEST_BYTES) {
    vkr_serve_close_client(serve, index);
    return;
  }
  uint64_t consumed = 0u;
  for (;;) {
    const char *start = (const char *)client->inbox.data + consumed;
    const uint64_t remaining = client->inbox.length - consumed;
    const char *newline = memchr(start, '\n', (size_t)remaining);
    if (!newline) {
      break;
    }
    const uint64_t length = (uint64_t)(newline - start);
    if (length) {
      vkr_serve_request(serve, index, start, length);
    }
    consumed += length + 1u;
    if (client->socket == VKR_LOCAL_SOCKET_INVALID) {
      return;
    }
  }
  if (consumed) {
    MemCopy(client->inbox.data, client->inbox.data + consumed,
            (size_t)(client->inbox.length - consumed));
    client->inbox.length -= consumed;
  }
}

// =============================================================================
// Main loop
// =============================================================================

vkr_internal bool8_t vkr_serve_enter(const char *root) {
#if defined(_WIN32)
  wchar_t wide[VKR_BAKERY_PATH_CAPACITY];
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, root, -1, wide,
                             (int)ArrayCount(wide)) > 0 &&
         _wchdir(wide) == 0;
#else
  return chdir(root) == 0;
#endif
}

vkr_internal bool8_t vkr_serve_idle(const VkrServe *serve) {
  for (uint32_t i = 0u; i < VKR_SERVE_MAX_CLIENTS; ++i) {
    if (serve->clients[i].socket != VKR_LOCAL_SOCKET_INVALID) {
      return false_v;
    }
  }
  return true_v;
}

vkr_internal void vkr_serve_loop(VkrServe *serve, uint64_t idle_exit_seconds) {
  float64_t idle_since = vkr_bakery_monotonic_seconds();
  while (!vkr_serve_signalled) {
    if (serve->watches_changed) {
      serve->watches_changed = false_v;
      if (!vkr_serve_restart_stream(serve)) {
        bool8_t any = false_v;
        for (uint32_t w = 0u; w < VKR_SERVE_MAX_WATCHES; ++w) {
          any = any || serve->watches[w].id != 0u;
        }
        if (any) {
          fprintf(stderr, "warning: file watching is unavailable on this "
                          "host\n");
        }
      }
    }
    VkrLocalSocketPoll fds[VKR_SERVE_MAX_CLIENTS + 2u];
    uint32_t indices[VKR_SERVE_MAX_CLIENTS];
    uint32_t count = 0u;
    fds[count++] = (VkrLocalSocketPoll){.socket = serve->listener};
    fds[count++] = (VkrLocalSocketPoll){.socket = serve->wake[0]};
    uint32_t client_count = 0u;
    for (uint32_t i = 0u; i < VKR_SERVE_MAX_CLIENTS; ++i) {
      if (serve->clients[i].socket != VKR_LOCAL_SOCKET_INVALID) {
        indices[client_count++] = i;
        fds[count++] = (VkrLocalSocketPoll){.socket = serve->clients[i].socket};
      }
    }
    const float64_t now = vkr_bakery_monotonic_seconds();
    float64_t next_due = now + 1.0;
    for (uint32_t w = 0u; w < VKR_SERVE_MAX_WATCHES; ++w) {
      if (serve->watches[w].id && serve->watches[w].pending &&
          serve->watches[w].due < next_due) {
        next_due = serve->watches[w].due;
      }
    }
    const int timeout_ms =
        next_due > now ? (int)((next_due - now) * 1000.0) + 1 : 0;
    /* A signal ends the wait without readable sockets. */
    const int32_t ready = vkr_local_socket_poll(fds, count, timeout_ms);
    if (ready < 0) {
      break;
    }
    if (ready > 0 && fds[1].readable) {
      char drain[64];
      uint64_t drained = 0u;
      while (vkr_local_socket_recv(serve->wake[0], drain, sizeof(drain),
                                   &drained) == VKR_LOCAL_SOCKET_OK) {
      }
    }
    if (ready > 0 && fds[0].readable) {
      vkr_serve_accept(serve);
    }
    for (uint32_t i = 0u; ready > 0 && i < client_count; ++i) {
      if (fds[i + 2u].readable) {
        vkr_serve_read(serve, indices[i]);
      }
    }
    const float64_t after = vkr_bakery_monotonic_seconds();
    vkr_serve_match_changes(serve, after);
    for (uint32_t w = 0u; w < VKR_SERVE_MAX_WATCHES; ++w) {
      VkrServeWatch *watch = &serve->watches[w];
      if (watch->id && watch->pending && watch->due <= after) {
        vkr_serve_fire_watch(serve, watch);
      }
    }
    if (!vkr_serve_idle(serve)) {
      idle_since = after;
    } else if (idle_exit_seconds &&
               after - idle_since >= (float64_t)idle_exit_seconds) {
      break;
    }
  }
}

int vkr_bakery_cmd_serve(VkrBakeryCli *cli) {
  VkrServe *serve = (VkrServe *)calloc(1u, sizeof(VkrServe));
  if (!serve) {
    return VKR_BAKERY_EXIT_ENVIRONMENT;
  }
  int code = VKR_BAKERY_EXIT_ENVIRONMENT;
  VkrThread builder = NULL;
  Arena *arena = arena_create(MB(4), KB(64));
  serve->allocator.ctx = arena;
  serve->listener = VKR_LOCAL_SOCKET_INVALID;
  serve->wake[0] = VKR_LOCAL_SOCKET_INVALID;
  serve->wake[1] = VKR_LOCAL_SOCKET_INVALID;
  for (uint32_t i = 0u; i < VKR_SERVE_MAX_CLIENTS; ++i) {
    serve->clients[i].socket = VKR_LOCAL_SOCKET_INVALID;
  }
  (void)snprintf(serve->root, sizeof(serve->root), "%s", cli->config.root);
  (void)snprintf(serve->program, sizeof(serve->program), "%s",
                 cli->config.self_path);
  if (!arena || !vkr_allocator_arena(&serve->allocator) ||
      !vkr_mutex_create(&serve->allocator, &serve->mutex) ||
      !vkr_mutex_create(&serve->allocator, &serve->write_mutex) ||
      !vkr_mutex_create(&serve->allocator, &serve->changes_mutex) ||
      !vkr_cond_create(&serve->allocator, &serve->ready) ||
      !vkr_serve_socket_path(cli, serve->socket_path,
                             sizeof(serve->socket_path)) ||
      !vkr_local_socket_pair(serve->wake)) {
    goto cleanup;
  }
  /* Neither a full wake socket nor an empty one may block. */
  (void)vkr_local_socket_set_nonblocking(serve->wake[0]);
  (void)vkr_local_socket_set_nonblocking(serve->wake[1]);
  /* Relative paths in requests resolve against the served root. */
  if (!vkr_serve_enter(serve->root)) {
    fprintf(stderr, "error VKR-CLI-0004: cannot enter %s\n", serve->root);
    goto cleanup;
  }
#if defined(SIGPIPE)
  signal(SIGPIPE, SIG_IGN);
#endif
  /* Commands keep the daemon's handlers: a signal stops the daemon, not only
     the command it is running. */
  vkr_bakery_signal_owner = vkr_serve_signal;
  vkr_bakery_install_cancel_signals();
#if defined(__APPLE__)
  serve->stream_queue =
      dispatch_queue_create("vkr.bakery.watch", DISPATCH_QUEUE_SERIAL);
#endif
  if (!vkr_serve_listen(serve)) {
    goto cleanup;
  }
  if (!vkr_thread_create(&serve->allocator, &builder, vkr_serve_builder,
                         serve)) {
    goto cleanup;
  }
  /* The editor and scripts read this one line to learn the socket. */
  VkrBakeryBuffer listening = {0};
  vkr_bakery_buffer_append_cstr(&listening,
                                "{\"v\":1,\"ev\":\"listening\",\"socket\":");
  vkr_bakery_json_write_string(&listening,
                               (String8){.str = (uint8_t *)serve->socket_path,
                                         .length = strlen(serve->socket_path)});
  vkr_bakery_buffer_append_cstr(&listening, ",\"root\":");
  vkr_bakery_json_write_string(
      &listening,
      (String8){.str = (uint8_t *)serve->root, .length = strlen(serve->root)});
  vkr_bakery_buffer_appendf(&listening, ",\"protocol\":%u}\n",
                            VKR_SERVE_PROTOCOL);
  if (!listening.failed) {
    fwrite(listening.data, 1u, (size_t)listening.length, stdout);
  }
  vkr_bakery_buffer_free(&listening);
  fflush(stdout);
  vkr_serve_loop(serve, cli->idle_exit_seconds);
  code = VKR_BAKERY_EXIT_OK;

cleanup:
  vkr_serve_stop_stream(serve);
  if (builder) {
    vkr_mutex_lock(serve->mutex);
    serve->stopping = true_v;
    vkr_atomic_bool_store(&vkr_bakery_cancel_requested, true_v,
                          VKR_MEMORY_ORDER_RELAXED);
    vkr_cond_signal(serve->ready);
    vkr_mutex_unlock(serve->mutex);
    (void)vkr_thread_join(builder);
  }
  for (uint32_t i = 0u; i < VKR_SERVE_MAX_CLIENTS; ++i) {
    if (serve->clients[i].socket != VKR_LOCAL_SOCKET_INVALID) {
      vkr_serve_close_client(serve, i);
    }
  }
  for (uint32_t i = 0u; i < serve->queue_count; ++i) {
    vkr_serve_arguments_free(&serve->queue[i].arguments);
  }
  for (uint32_t w = 0u; w < VKR_SERVE_MAX_WATCHES; ++w) {
    vkr_serve_arguments_free(&serve->watches[w].arguments);
  }
  if (serve->listener != VKR_LOCAL_SOCKET_INVALID) {
    vkr_local_socket_close(serve->listener);
    (void)vkr_local_socket_remove_path(serve->socket_path);
  }
  vkr_local_socket_close(serve->wake[0]);
  vkr_local_socket_close(serve->wake[1]);
#if defined(__APPLE__)
  if (serve->stream_queue) {
    dispatch_release(serve->stream_queue);
  }
#endif
  if (arena) {
    arena_destroy(arena);
  }
  free(serve);
  return code;
}

// =============================================================================
// send
// =============================================================================

/* Prints every line until the reply to this request; a watch keeps
   printing its changes until the daemon closes the connection. */
int vkr_bakery_cmd_send(VkrBakeryCli *cli) {
  if (cli->positional_count != 1u) {
    fprintf(stderr, "error VKR-CLI-0001: send needs one request JSON "
                    "argument\n");
    return VKR_BAKERY_EXIT_USAGE;
  }
  char socket_path[VKR_BAKERY_PATH_CAPACITY];
  if (!vkr_serve_socket_path(cli, socket_path, sizeof(socket_path))) {
    return VKR_BAKERY_EXIT_USAGE;
  }
  Arena *arena = arena_create(MB(1), KB(64));
  const char *request_text = cli->positional[0];
  const VkrBakeryJson *request =
      arena ? vkr_bakery_json_parse(arena, (const uint8_t *)request_text,
                                    strlen(request_text), 16u, NULL)
            : NULL;
  int64_t id = 0;
  if (!request || !vkr_bakery_json_get_int(request, "id", &id)) {
    if (arena) {
      arena_destroy(arena);
    }
    fprintf(stderr, "error VKR-CLI-0001: the request must be JSON with an "
                    "id\n");
    return VKR_BAKERY_EXIT_USAGE;
  }
  const bool8_t follow =
      vkr_bakery_json_is_string(vkr_bakery_json_get(request, "req"), "watch");
#if defined(SIGPIPE)
  signal(SIGPIPE, SIG_IGN);
#endif
  VkrLocalSocket connection = VKR_LOCAL_SOCKET_INVALID;
  if (!vkr_local_socket_connect(socket_path, &connection)) {
    arena_destroy(arena);
    fprintf(stderr, "error VKR-CLI-0005: no daemon at %s\n", socket_path);
    return VKR_BAKERY_EXIT_ENVIRONMENT;
  }
  int code = VKR_BAKERY_EXIT_ENVIRONMENT;
  VkrBakeryBuffer line = {0};
  vkr_bakery_buffer_append_cstr(&line, request_text);
  vkr_bakery_buffer_append(&line, "\n", 1u);
  if (line.failed ||
      !vkr_local_socket_send_all(connection, line.data, line.length)) {
    goto done;
  }
  VkrBakeryBuffer inbox = {0};
  char needle[64];
  (void)snprintf(needle, sizeof(needle), "\"req\":%lld,", (long long)id);
  for (;;) {
    char chunk[4096];
    uint64_t received = 0u;
    if (vkr_local_socket_recv(connection, chunk, sizeof(chunk), &received) !=
        VKR_LOCAL_SOCKET_OK) {
      code = follow ? VKR_BAKERY_EXIT_OK : VKR_BAKERY_EXIT_ENVIRONMENT;
      break;
    }
    vkr_bakery_buffer_append(&inbox, chunk, received);
    bool8_t finished = false_v;
    uint64_t consumed = 0u;
    for (;;) {
      const char *start = (const char *)inbox.data + consumed;
      const char *newline =
          memchr(start, '\n', (size_t)(inbox.length - consumed));
      if (!newline) {
        break;
      }
      const uint64_t length = (uint64_t)(newline - start) + 1u;
      fwrite(start, 1u, (size_t)length, stdout);
      fflush(stdout);
      const bool8_t reply =
          vkr_serve_find(start, length, "\"ev\":\"reply\"") != NULL &&
          vkr_serve_find(start, length, needle) != NULL;
      if (reply && !finished) {
        const char *exit_field = vkr_serve_find(start, length, "\"exit\":");
        code = exit_field ? atoi(exit_field + 7) : VKR_BAKERY_EXIT_FAILED;
        finished = !follow || code != 0;
      }
      consumed += length;
    }
    if (consumed) {
      MemCopy(inbox.data, inbox.data + consumed,
              (size_t)(inbox.length - consumed));
      inbox.length -= consumed;
    }
    if (finished) {
      break;
    }
  }
  vkr_bakery_buffer_free(&inbox);
done:
  vkr_bakery_buffer_free(&line);
  vkr_local_socket_close(connection);
  arena_destroy(arena);
  return code;
}
