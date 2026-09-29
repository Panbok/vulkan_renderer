#include "editor_bakery_service.h"
#include "editor_install.h"

#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "core/vkr_threads.h"
#include "platform/vkr_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define EDITOR_BAKERY_SERVICE_WATCHES 16u
#define EDITOR_BAKERY_SERVICE_PATHS 16u
#define EDITOR_BAKERY_SERVICE_ARGUMENTS 24u
#define EDITOR_BAKERY_SERVICE_CHANGES 64u
#define EDITOR_BAKERY_SERVICE_INBOX (256u * 1024u)
/* Restarts allowed within one window before the service gives up. */
#define EDITOR_BAKERY_SERVICE_RESTARTS 5u
#define EDITOR_BAKERY_SERVICE_RESTART_WINDOW_SECONDS 60.0
#define EDITOR_BAKERY_SERVICE_CONNECT_SECONDS 10.0

typedef struct EditorBakeryWatch {
  uint32_t handle; /* EDITOR_BAKERY_SERVICE_NONE when free. */
  char paths[EDITOR_BAKERY_SERVICE_PATHS][EDITOR_BAKERY_SERVICE_PATH];
  uint32_t path_count;
  char arguments[EDITOR_BAKERY_SERVICE_ARGUMENTS][EDITOR_BAKERY_SERVICE_PATH];
  uint32_t argument_count;
  /* Daemon-side identity; zero until the current daemon confirms it. */
  uint64_t request;
  uint32_t daemon_watch;
  char changes[EDITOR_BAKERY_SERVICE_CHANGES][EDITOR_BAKERY_SERVICE_PATH];
  uint32_t change_count;
  EditorBakeryRebuild building;
  EditorBakeryRebuild finished;
  bool8_t has_finished;
  /* A reported change whose rebuild has not replied. A restarted daemon
     reruns it as a `run` request tagged `rerun`. */
  bool8_t pending;
  uint64_t rerun;
} EditorBakeryWatch;

struct EditorBakeryService {
  VkrAllocator *allocator;
  VkrThread supervisor;
  VkrAtomicBool stopping;
  VkrAtomicBool exited;
  int32_t exit_code;
  bool8_t available;
  bool8_t given_up;
  /* The current daemon accepted this editor once; a closed connection then
     means it is exiting. */
  bool8_t connected;
  int socket_fd;
  float64_t started_at;
  float64_t lost_at;
  float64_t restart_at;
  float64_t restart_times[EDITOR_BAKERY_SERVICE_RESTARTS];
  uint32_t restart_count;
  uint64_t next_request;
  char socket_path[EDITOR_BAKERY_SERVICE_PATH];
  char log_path[EDITOR_BAKERY_SERVICE_PATH];
  char root[EDITOR_BAKERY_SERVICE_PATH];
  EditorBakeryWatch watches[EDITOR_BAKERY_SERVICE_WATCHES];
  uint32_t next_handle;
  char inbox[EDITOR_BAKERY_SERVICE_INBOX];
  uint32_t inbox_length;
};

#if defined(_WIN32)

/* `vkr_bakery serve` has no Windows transport yet; watches are unavailable
   and Bakery recipes keep running as explicit jobs. */
EditorBakeryService *editor_bakery_service_create(VkrAllocator *allocator) {
  (void)allocator;
  return NULL;
}

void editor_bakery_service_destroy(EditorBakeryService *service) {
  (void)service;
}

void editor_bakery_service_update(EditorBakeryService *service) {
  (void)service;
}

uint32_t editor_bakery_service_watch(EditorBakeryService *service,
                                     const char *const *paths,
                                     uint32_t path_count,
                                     const char *const *argv,
                                     uint32_t argument_count) {
  (void)service;
  (void)paths;
  (void)path_count;
  (void)argv;
  (void)argument_count;
  return EDITOR_BAKERY_SERVICE_NONE;
}

void editor_bakery_service_unwatch(EditorBakeryService *service,
                                   uint32_t handle) {
  (void)service;
  (void)handle;
}

uint32_t editor_bakery_service_take_changes(
    EditorBakeryService *service, uint32_t handle,
    char (*out)[EDITOR_BAKERY_SERVICE_PATH], uint32_t capacity) {
  (void)service;
  (void)handle;
  (void)out;
  (void)capacity;
  return 0u;
}

bool8_t editor_bakery_service_take_rebuild(EditorBakeryService *service,
                                           uint32_t handle,
                                           EditorBakeryRebuild *out) {
  (void)service;
  (void)handle;
  (void)out;
  return false_v;
}

#else

// =============================================================================
// Child supervision
// =============================================================================

static bool8_t editor_bakery_service_cancelled(void *context) {
  EditorBakeryService *service = context;
  return vkr_atomic_bool_load(&service->stopping, VKR_MEMORY_ORDER_ACQUIRE);
}

/* Runs the daemon until it exits; the UI thread observes `exited`. */
static void *editor_bakery_service_supervise(void *context) {
  EditorBakeryService *service = context;
  const char *arguments[] = {
      "serve",       "--root", service->root, "--socket", service->socket_path,
      "--idle-exit", "30"};
  const VkrPlatformProcessConfig config = {
      .executable = vkr_editor_tool_path(VKR_EDITOR_TOOL_BAKERY),
      .arguments = arguments,
      .argument_count = ArrayCount(arguments),
      .stdout_path = service->log_path,
      .stderr_path = service->log_path,
      .append_output = true_v,
      .termination_grace_ms = 2000u,
      .terminate_process_tree = true_v,
      .hidden = true_v,
      .is_cancelled = editor_bakery_service_cancelled,
      .cancel_context = service,
  };
  int32_t code = -1;
  bool8_t timed_out = false_v;
  if (!vkr_platform_process_run(&config, &code, &timed_out)) {
    code = -1;
  }
  service->exit_code = code;
  vkr_atomic_bool_store(&service->exited, true_v, VKR_MEMORY_ORDER_RELEASE);
  return NULL;
}

static void editor_bakery_service_disconnect(EditorBakeryService *service) {
  if (service->socket_fd >= 0) {
    close(service->socket_fd);
    service->socket_fd = -1;
  }
  service->inbox_length = 0u;
  service->lost_at = vkr_platform_get_absolute_time();
  for (uint32_t i = 0u; i < EDITOR_BAKERY_SERVICE_WATCHES; ++i) {
    service->watches[i].request = 0u;
    service->watches[i].daemon_watch = 0u;
  }
}

static bool8_t editor_bakery_service_start(EditorBakeryService *service) {
  (void)unlink(service->socket_path);
  service->connected = false_v;
  vkr_atomic_bool_store(&service->stopping, false_v, VKR_MEMORY_ORDER_RELEASE);
  vkr_atomic_bool_store(&service->exited, false_v, VKR_MEMORY_ORDER_RELEASE);
  if (!vkr_thread_create(service->allocator, &service->supervisor,
                         editor_bakery_service_supervise, service)) {
    return false_v;
  }
  service->started_at = vkr_platform_get_absolute_time();
  return true_v;
}

static bool8_t editor_bakery_service_send(EditorBakeryService *service,
                                          const char *line, uint64_t length) {
  while (length && service->socket_fd >= 0) {
    const ssize_t sent = send(service->socket_fd, line, (size_t)length, 0);
    if (sent < 0 && (errno == EAGAIN || errno == EINTR)) {
      continue;
    }
    if (sent <= 0) {
      return false_v;
    }
    line += sent;
    length -= (uint64_t)sent;
  }
  return length == 0u;
}

/* Appends `text` as a JSON string. Paths and arguments are UTF-8. */
static uint32_t editor_bakery_service_quote(char *out, uint32_t capacity,
                                            uint32_t length, const char *text) {
  if (length + 1u < capacity) {
    out[length++] = '"';
  }
  for (const char *c = text; *c && length + 8u < capacity; ++c) {
    if (*c == '"' || *c == '\\') {
      out[length++] = '\\';
      out[length++] = *c;
    } else if ((uint8_t)*c < 0x20u) {
      length += (uint32_t)snprintf(out + length, capacity - length, "\\u%04x",
                                   (uint32_t)(uint8_t)*c);
    } else {
      out[length++] = *c;
    }
  }
  if (length + 1u < capacity) {
    out[length++] = '"';
  }
  return length;
}

/* Appends `,"argv":[...]` with the watch's rebuild command. */
static uint32_t editor_bakery_service_arguments(const EditorBakeryWatch *watch,
                                                char *line, uint32_t capacity,
                                                uint32_t length) {
  length += (uint32_t)snprintf(line + length, capacity - length, ",\"argv\":[");
  for (uint32_t i = 0u; i < watch->argument_count && length < capacity; ++i) {
    if (i && length + 1u < capacity) {
      line[length++] = ',';
    }
    length = editor_bakery_service_quote(line, capacity, length,
                                         watch->arguments[i]);
  }
  if (length + 1u < capacity) {
    line[length++] = ']';
  }
  return length;
}

static void editor_bakery_service_register(EditorBakeryService *service,
                                           EditorBakeryWatch *watch) {
  static char line[64u * 1024u];
  watch->request = ++service->next_request;
  uint32_t length = (uint32_t)snprintf(
      line, sizeof(line), "{\"v\":1,\"id\":%llu,\"req\":\"watch\",\"paths\":[",
      (unsigned long long)watch->request);
  for (uint32_t i = 0u; i < watch->path_count; ++i) {
    if (i && length + 1u < sizeof(line)) {
      line[length++] = ',';
    }
    length = editor_bakery_service_quote(line, sizeof(line), length,
                                         watch->paths[i]);
  }
  if (length + 2u < sizeof(line)) {
    line[length++] = ']';
  }
  if (watch->argument_count) {
    length = editor_bakery_service_arguments(watch, line, sizeof(line), length);
  }
  length += (uint32_t)snprintf(line + length, sizeof(line) - length, "}\n");
  if (length >= sizeof(line) ||
      !editor_bakery_service_send(service, line, length)) {
    editor_bakery_service_disconnect(service);
    return;
  }
  if (!watch->pending || !watch->argument_count) {
    return;
  }
  /* The previous daemon stopped before this watch's rebuild replied. */
  watch->rerun = ++service->next_request;
  MemZero(&watch->building, sizeof(watch->building));
  length = (uint32_t)snprintf(line, sizeof(line),
                              "{\"v\":1,\"id\":%llu,\"req\":\"run\"",
                              (unsigned long long)watch->rerun);
  length = editor_bakery_service_arguments(watch, line, sizeof(line), length);
  length += (uint32_t)snprintf(line + length, sizeof(line) - length, "}\n");
  if (length >= sizeof(line) ||
      !editor_bakery_service_send(service, line, length)) {
    editor_bakery_service_disconnect(service);
  }
}

static void editor_bakery_service_connect(EditorBakeryService *service) {
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return;
  }
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  snprintf(address.sun_path, sizeof(address.sun_path), "%s",
           service->socket_path);
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    close(fd);
    return;
  }
#if defined(SO_NOSIGPIPE)
  const int one = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  (void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  service->socket_fd = fd;
  service->connected = true_v;
  for (uint32_t i = 0u; i < EDITOR_BAKERY_SERVICE_WATCHES; ++i) {
    if (service->watches[i].handle) {
      editor_bakery_service_register(service, &service->watches[i]);
    }
  }
}

// =============================================================================
// Events
// =============================================================================

/* Integer member of a compact event line; false when absent. */
static bool8_t editor_bakery_service_field(const char *line, uint32_t length,
                                           const char *name, int64_t *out) {
  char key[32];
  const int key_length = snprintf(key, sizeof(key), "\"%s\":", name);
  const char *found = NULL;
  for (const char *c = line; c + key_length <= line + length; ++c) {
    if (memcmp(c, key, (size_t)key_length) == 0) {
      found = c + key_length;
      break;
    }
  }
  if (!found || (*found != '-' && (*found < '0' || *found > '9'))) {
    return false_v;
  }
  *out = strtoll(found, NULL, 10);
  return true_v;
}

static bool8_t editor_bakery_service_is(const char *line, uint32_t length,
                                        const char *event) {
  char key[48];
  const int key_length = snprintf(key, sizeof(key), "\"ev\":\"%s\"", event);
  for (const char *c = line; c + key_length <= line + length; ++c) {
    if (memcmp(c, key, (size_t)key_length) == 0) {
      return true_v;
    }
  }
  return false_v;
}

/* Decodes the JSON string starting at `text` (after its opening quote). */
static const char *editor_bakery_service_decode(const char *text,
                                                const char *end, char *out,
                                                uint32_t capacity) {
  uint32_t length = 0u;
  while (text < end && *text != '"') {
    uint32_t code = (uint8_t)*text++;
    if (code == '\\' && text < end) {
      const char escape = *text++;
      code = escape == 'n'   ? '\n'
             : escape == 't' ? '\t'
             : escape == 'r' ? '\r'
             : escape == 'b' ? '\b'
             : escape == 'f' ? '\f'
                             : (uint8_t)escape;
      if (escape == 'u' && end - text >= 4) {
        char hex[5] = {text[0], text[1], text[2], text[3], 0};
        code = (uint32_t)strtoul(hex, NULL, 16);
        text += 4;
        if (code >= 0xD800u && code < 0xDC00u && end - text >= 6 &&
            text[0] == '\\' && text[1] == 'u') {
          char low_hex[5] = {text[2], text[3], text[4], text[5], 0};
          const uint32_t low = (uint32_t)strtoul(low_hex, NULL, 16);
          code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
          text += 6;
        }
        /* Re-encode as UTF-8. */
        uint8_t bytes[4];
        uint32_t count = 0u;
        if (code < 0x80u) {
          bytes[count++] = (uint8_t)code;
        } else if (code < 0x800u) {
          bytes[count++] = (uint8_t)(0xC0u | (code >> 6));
          bytes[count++] = (uint8_t)(0x80u | (code & 0x3Fu));
        } else if (code < 0x10000u) {
          bytes[count++] = (uint8_t)(0xE0u | (code >> 12));
          bytes[count++] = (uint8_t)(0x80u | ((code >> 6) & 0x3Fu));
          bytes[count++] = (uint8_t)(0x80u | (code & 0x3Fu));
        } else {
          bytes[count++] = (uint8_t)(0xF0u | (code >> 18));
          bytes[count++] = (uint8_t)(0x80u | ((code >> 12) & 0x3Fu));
          bytes[count++] = (uint8_t)(0x80u | ((code >> 6) & 0x3Fu));
          bytes[count++] = (uint8_t)(0x80u | (code & 0x3Fu));
        }
        for (uint32_t i = 0u; i < count && length + 1u < capacity; ++i) {
          out[length++] = (char)bytes[i];
        }
        continue;
      }
    }
    if (length + 1u < capacity) {
      out[length++] = (char)code;
    }
  }
  out[length] = 0;
  return text < end ? text + 1 : end;
}

static EditorBakeryWatch *
editor_bakery_service_find(EditorBakeryService *service, const char *line,
                           uint32_t length) {
  int64_t value = 0;
  if (editor_bakery_service_field(line, length, "watch", &value)) {
    for (uint32_t i = 0u; i < EDITOR_BAKERY_SERVICE_WATCHES; ++i) {
      EditorBakeryWatch *watch = &service->watches[i];
      if (watch->handle && watch->daemon_watch == (uint32_t)value) {
        return watch;
      }
    }
  } else if (editor_bakery_service_field(line, length, "req", &value)) {
    for (uint32_t i = 0u; i < EDITOR_BAKERY_SERVICE_WATCHES; ++i) {
      EditorBakeryWatch *watch = &service->watches[i];
      if (watch->handle && watch->rerun && watch->rerun == (uint64_t)value) {
        return watch;
      }
    }
  }
  return NULL;
}

static void editor_bakery_service_changed(EditorBakeryWatch *watch,
                                          const char *line, uint32_t length) {
  const char *end = line + length;
  const char *cursor = strstr(line, "\"paths\":[");
  if (!cursor || cursor >= end) {
    return;
  }
  cursor += 9;
  while (cursor < end && *cursor != ']') {
    if (*cursor != '"') {
      cursor += 1;
      continue;
    }
    char path[EDITOR_BAKERY_SERVICE_PATH];
    cursor = editor_bakery_service_decode(cursor + 1, end, path, sizeof(path));
    if (watch->change_count < EDITOR_BAKERY_SERVICE_CHANGES) {
      snprintf(watch->changes[watch->change_count++],
               EDITOR_BAKERY_SERVICE_PATH, "%s", path);
    }
  }
}

/* Errors of a watch rebuild reach the Console; warnings stay in the daemon
   log. */
static void editor_bakery_service_diagnostic(const char *line,
                                             uint32_t length) {
  const char *end = line + length;
  const char *severity = strstr(line, "\"severity\":\"error\"");
  if (!severity || severity >= end) {
    return;
  }
  char message[512] = {0};
  char source[512] = {0};
  const char *field = strstr(line, "\"message\":\"");
  if (field && field < end) {
    (void)editor_bakery_service_decode(field + 11, end, message,
                                       sizeof(message));
  }
  field = strstr(line, "\"source\":\"");
  if (field && field < end) {
    (void)editor_bakery_service_decode(field + 10, end, source, sizeof(source));
  }
  log_error("Bakery watch: %s %s", source, message);
}

static void editor_bakery_service_event(EditorBakeryService *service,
                                        const char *line, uint32_t length) {
  int64_t value = 0;
  if (editor_bakery_service_is(line, length, "reply") &&
      editor_bakery_service_field(line, length, "req", &value)) {
    for (uint32_t i = 0u; i < EDITOR_BAKERY_SERVICE_WATCHES; ++i) {
      EditorBakeryWatch *watch = &service->watches[i];
      int64_t id = 0;
      if (watch->handle && watch->request == (uint64_t)value) {
        if (editor_bakery_service_field(line, length, "watch", &id)) {
          watch->daemon_watch = (uint32_t)id;
        } else {
          log_warn("Bakery daemon rejected a watch: %.*s", (int)length, line);
        }
        return;
      }
    }
  }
  EditorBakeryWatch *watch = editor_bakery_service_find(service, line, length);
  if (!watch) {
    return;
  }
  if (editor_bakery_service_is(line, length, "changed")) {
    editor_bakery_service_changed(watch, line, length);
    watch->pending = watch->argument_count != 0u;
  } else if (editor_bakery_service_is(line, length, "summary")) {
    int64_t ok = 0;
    int64_t failed = 0;
    int64_t cancelled = 0;
    int64_t cached = 0;
    (void)editor_bakery_service_field(line, length, "ok", &ok);
    (void)editor_bakery_service_field(line, length, "failed", &failed);
    (void)editor_bakery_service_field(line, length, "cancelled", &cancelled);
    (void)editor_bakery_service_field(line, length, "cached", &cached);
    watch->building.actions += (uint32_t)(ok + failed + cancelled);
    watch->building.cached += (uint32_t)cached;
    watch->building.failed += (uint32_t)failed;
  } else if (editor_bakery_service_is(line, length, "diag")) {
    editor_bakery_service_diagnostic(line, length);
  } else if (editor_bakery_service_is(line, length, "reply") &&
             editor_bakery_service_field(line, length, "exit", &value)) {
    watch->building.exit_code = (int32_t)value;
    watch->finished = watch->building;
    watch->has_finished = true_v;
    watch->pending = false_v;
    watch->rerun = 0u;
    MemZero(&watch->building, sizeof(watch->building));
  }
}

static void editor_bakery_service_read(EditorBakeryService *service) {
  for (;;) {
    const uint32_t space = EDITOR_BAKERY_SERVICE_INBOX - service->inbox_length;
    if (!space) {
      /* A line longer than the inbox cannot be one of the daemon's events. */
      service->inbox_length = 0u;
      continue;
    }
    const ssize_t received = recv(
        service->socket_fd, service->inbox + service->inbox_length, space, 0);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received <= 0) {
      editor_bakery_service_disconnect(service);
      return;
    }
    service->inbox_length += (uint32_t)received;
    uint32_t consumed = 0u;
    for (;;) {
      const char *start = service->inbox + consumed;
      const char *newline =
          memchr(start, '\n', service->inbox_length - consumed);
      if (!newline) {
        break;
      }
      editor_bakery_service_event(service, start, (uint32_t)(newline - start));
      consumed += (uint32_t)(newline - start) + 1u;
    }
    if (consumed) {
      MemCopy(service->inbox, service->inbox + consumed,
              service->inbox_length - consumed);
      service->inbox_length -= consumed;
    }
  }
}

// =============================================================================
// Public API
// =============================================================================

EditorBakeryService *editor_bakery_service_create(VkrAllocator *allocator) {
  EditorBakeryService *service = vkr_allocator_alloc(
      allocator, sizeof(*service), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!service) {
    return NULL;
  }
  MemZero(service, sizeof(*service));
  service->allocator = allocator;
  service->socket_fd = -1;
  service->available = true_v;
  const char *directory = getenv("TMPDIR");
  if (!directory || !directory[0]) {
    directory = "/tmp";
  }
  const uint32_t pid = vkr_platform_get_process_id();
  const uint64_t length = strlen(directory);
  const char *separator = directory[length - 1u] == '/' ? "" : "/";
  snprintf(service->root, sizeof(service->root), "%s", directory);
  const int socket_length =
      snprintf(service->socket_path, sizeof(service->socket_path),
               "%s%svkr-editor-%u.sock", directory, separator, pid);
  snprintf(service->log_path, sizeof(service->log_path),
           "%s%svkr-editor-%u.bakery.log", directory, separator, pid);
  if (socket_length <= 0 ||
      (uint64_t)socket_length >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
    log_warn("Bakery daemon disabled: the socket path is too long");
    service->available = false_v;
  }
  return service;
}

void editor_bakery_service_destroy(EditorBakeryService *service) {
  if (!service) {
    return;
  }
  if (service->socket_fd >= 0) {
    static const char shutdown[] =
        "{\"v\":1,\"id\":1000000000,\"req\":\"shutdown\"}\n";
    (void)editor_bakery_service_send(service, shutdown, sizeof(shutdown) - 1u);
  }
  vkr_atomic_bool_store(&service->stopping, true_v, VKR_MEMORY_ORDER_RELEASE);
  if (service->supervisor) {
    (void)vkr_thread_join(service->supervisor);
    (void)vkr_thread_destroy(service->allocator, &service->supervisor);
  }
  editor_bakery_service_disconnect(service);
  (void)unlink(service->socket_path);
  (void)unlink(service->log_path);
  vkr_allocator_free(service->allocator, service, sizeof(*service),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
}

static bool8_t
editor_bakery_service_has_watches(const EditorBakeryService *service) {
  for (uint32_t i = 0u; i < EDITOR_BAKERY_SERVICE_WATCHES; ++i) {
    if (service->watches[i].handle) {
      return true_v;
    }
  }
  return false_v;
}

/* Records a restart; false once restarts exceed their window budget. */
static bool8_t editor_bakery_service_allow_restart(EditorBakeryService *service,
                                                   float64_t now) {
  uint32_t recent = 0u;
  for (uint32_t i = 0u; i < EDITOR_BAKERY_SERVICE_RESTARTS; ++i) {
    recent += service->restart_times[i] > 0.0 &&
                      now - service->restart_times[i] <
                          EDITOR_BAKERY_SERVICE_RESTART_WINDOW_SECONDS
                  ? 1u
                  : 0u;
  }
  if (recent >= EDITOR_BAKERY_SERVICE_RESTARTS) {
    return false_v;
  }
  service->restart_times[service->restart_count++ %
                         EDITOR_BAKERY_SERVICE_RESTARTS] = now;
  return true_v;
}

void editor_bakery_service_update(EditorBakeryService *service) {
  if (!service || !service->available || service->given_up) {
    return;
  }
  const float64_t now = vkr_platform_get_absolute_time();
  if (service->supervisor &&
      vkr_atomic_bool_load(&service->exited, VKR_MEMORY_ORDER_ACQUIRE)) {
    (void)vkr_thread_join(service->supervisor);
    (void)vkr_thread_destroy(service->allocator, &service->supervisor);
    editor_bakery_service_disconnect(service);
    if (!editor_bakery_service_has_watches(service)) {
      return;
    }
    if (!editor_bakery_service_allow_restart(service, now)) {
      log_error("Bakery daemon stopped repeatedly (last exit %d); file "
                "watching is off until the editor restarts. Log: %s",
                service->exit_code, service->log_path);
      service->given_up = true_v;
      return;
    }
    log_warn("Bakery daemon stopped (exit %d); restarting. Log: %s",
             service->exit_code, service->log_path);
    service->restart_at = now + 1.0;
  }
  if (!service->supervisor) {
    if (!editor_bakery_service_has_watches(service) ||
        now < service->restart_at) {
      return;
    }
    if (!editor_bakery_service_start(service)) {
      log_error("Bakery daemon could not start");
      service->given_up = true_v;
    }
    return;
  }
  if (service->socket_fd < 0 && service->connected) {
    /* A daemon that closed the connection is exiting; one that stays alive
       without its editor is stopped, and the exit path restarts it. */
    if (now - service->lost_at > EDITOR_BAKERY_SERVICE_CONNECT_SECONDS) {
      vkr_atomic_bool_store(&service->stopping, true_v,
                            VKR_MEMORY_ORDER_RELEASE);
    }
    return;
  }
  if (service->socket_fd < 0) {
    editor_bakery_service_connect(service);
    if (service->socket_fd < 0 &&
        now - service->started_at > EDITOR_BAKERY_SERVICE_CONNECT_SECONDS) {
      log_error("Bakery daemon did not accept connections; see %s",
                service->log_path);
      vkr_atomic_bool_store(&service->stopping, true_v,
                            VKR_MEMORY_ORDER_RELEASE);
      (void)vkr_thread_join(service->supervisor);
      (void)vkr_thread_destroy(service->allocator, &service->supervisor);
      service->given_up = true_v;
    }
    return;
  }
  editor_bakery_service_read(service);
}

uint32_t editor_bakery_service_watch(EditorBakeryService *service,
                                     const char *const *paths,
                                     uint32_t path_count,
                                     const char *const *argv,
                                     uint32_t argument_count) {
  if (!service || !service->available || service->given_up || !path_count ||
      path_count > EDITOR_BAKERY_SERVICE_PATHS ||
      argument_count > EDITOR_BAKERY_SERVICE_ARGUMENTS) {
    return EDITOR_BAKERY_SERVICE_NONE;
  }
  EditorBakeryWatch *watch = NULL;
  for (uint32_t i = 0u; i < EDITOR_BAKERY_SERVICE_WATCHES && !watch; ++i) {
    watch = service->watches[i].handle ? NULL : &service->watches[i];
  }
  if (!watch) {
    return EDITOR_BAKERY_SERVICE_NONE;
  }
  MemZero(watch, sizeof(*watch));
  for (uint32_t i = 0u; i < path_count; ++i) {
    snprintf(watch->paths[i], EDITOR_BAKERY_SERVICE_PATH, "%s", paths[i]);
  }
  for (uint32_t i = 0u; i < argument_count; ++i) {
    snprintf(watch->arguments[i], EDITOR_BAKERY_SERVICE_PATH, "%s", argv[i]);
  }
  watch->path_count = path_count;
  watch->argument_count = argument_count;
  watch->handle = ++service->next_handle;
  if (service->socket_fd >= 0) {
    editor_bakery_service_register(service, watch);
  }
  return watch->handle;
}

static EditorBakeryWatch *
editor_bakery_service_handle(EditorBakeryService *service, uint32_t handle) {
  for (uint32_t i = 0u; service && handle && i < EDITOR_BAKERY_SERVICE_WATCHES;
       ++i) {
    if (service->watches[i].handle == handle) {
      return &service->watches[i];
    }
  }
  return NULL;
}

void editor_bakery_service_unwatch(EditorBakeryService *service,
                                   uint32_t handle) {
  EditorBakeryWatch *watch = editor_bakery_service_handle(service, handle);
  if (!watch) {
    return;
  }
  if (watch->daemon_watch && service->socket_fd >= 0) {
    char line[128];
    const int length = snprintf(
        line, sizeof(line),
        "{\"v\":1,\"id\":%llu,\"req\":\"unwatch\",\"watch\":%u}\n",
        (unsigned long long)++service->next_request, watch->daemon_watch);
    (void)editor_bakery_service_send(service, line, (uint64_t)length);
  }
  MemZero(watch, sizeof(*watch));
}

uint32_t editor_bakery_service_take_changes(
    EditorBakeryService *service, uint32_t handle,
    char (*out)[EDITOR_BAKERY_SERVICE_PATH], uint32_t capacity) {
  EditorBakeryWatch *watch = editor_bakery_service_handle(service, handle);
  if (!watch || !watch->change_count) {
    return 0u;
  }
  const uint32_t count = Min(capacity, watch->change_count);
  MemCopy(out, watch->changes, count * sizeof(watch->changes[0]));
  MemCopy(watch->changes, watch->changes[count],
          (watch->change_count - count) * sizeof(watch->changes[0]));
  watch->change_count -= count;
  return count;
}

bool8_t editor_bakery_service_take_rebuild(EditorBakeryService *service,
                                           uint32_t handle,
                                           EditorBakeryRebuild *out) {
  EditorBakeryWatch *watch = editor_bakery_service_handle(service, handle);
  if (!watch || !watch->has_finished) {
    return false_v;
  }
  *out = watch->finished;
  watch->has_finished = false_v;
  return true_v;
}

#endif
