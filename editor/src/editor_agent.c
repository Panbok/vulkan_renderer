#include "editor_agent.h"

#include "editor_internal.h"
#include "editor_ops.h"

#include "core/logger.h"
#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

/* Clients a listener serves at once, the longest request line and the
 * requests that may wait behind the active one. */
#define AGENT_CLIENT_MAX 4u
#define AGENT_LINE_MAX MB(1)
#define AGENT_QUEUE_MAX 64u
#define AGENT_ARENA_RESERVE MB(64)
/* A client slot for requests the editor submits itself. */
#define AGENT_CLIENT_SELF UINT32_MAX

typedef struct AgentClient {
  int fd;
  /* Increments when the slot takes a new client, so a response never
     reaches a later client in the same slot. */
  uint32_t generation;
  uint8_t *in;
  uint64_t in_length;
  uint64_t in_capacity;
  uint8_t *out;
  uint64_t out_length;
  uint64_t out_capacity;
} AgentClient;

typedef struct AgentQueued {
  uint32_t client;
  uint32_t generation;
  char *line;
  uint64_t length;
} AgentQueued;

struct VkrEditorAgent {
  VkrAllocator *allocator;
  VkrEditorOps *ops;
  int listen_fd;
  char socket_path[256];
  char status[320];
  AgentClient clients[AGENT_CLIENT_MAX];
  AgentQueued queue[AGENT_QUEUE_MAX];
  uint32_t queue_head;
  uint32_t queue_count;
  /* The request in progress: its client, raw id text and call. */
  bool8_t active;
  AgentQueued current;
  String8 current_id;
  Arena *arena;
  VkrEditorOpCall call;
};

// =============================================================================
// Buffers
// =============================================================================

static bool8_t agent_reserve(uint8_t **data, uint64_t *capacity,
                             uint64_t needed) {
  if (needed <= *capacity) {
    return true_v;
  }
  uint64_t next = Max(*capacity * 2u, (uint64_t)4096u);
  while (next < needed) {
    next *= 2u;
  }
  uint8_t *grown = realloc(*data, next);
  if (!grown) {
    return false_v;
  }
  *data = grown;
  *capacity = next;
  return true_v;
}

static void agent_client_close(AgentClient *client) {
#if !defined(_WIN32)
  if (client->fd >= 0) {
    close(client->fd);
  }
#endif
  client->fd = -1;
  client->in_length = 0u;
  client->out_length = 0u;
}

static void agent_send(VkrEditorAgent *agent, uint32_t slot,
                       uint32_t generation, String8 text) {
  if (slot == AGENT_CLIENT_SELF) {
    /* Scripts read the whole result from this line; the Console keeps its
       own record length. */
    log_info("[agent] %.*s", (int)Min(text.length, (uint64_t)INT32_MAX),
             text.str);
    return;
  }
  AgentClient *client = &agent->clients[slot];
  if (client->fd < 0 || client->generation != generation) {
    return;
  }
  if (!agent_reserve(&client->out, &client->out_capacity,
                     client->out_length + text.length + 1u)) {
    agent_client_close(client);
    return;
  }
  MemCopy(client->out + client->out_length, text.str, text.length);
  client->out_length += text.length;
  client->out[client->out_length++] = '\n';
}

// =============================================================================
// Responses
// =============================================================================

/* Writes `{"v":1,"id":<id>,"ok":...}` for the current request. The id is
   the request's own JSON text, so a string or number returns unchanged. */
static void agent_respond(VkrEditorAgent *agent, uint32_t slot,
                          uint32_t generation, String8 id,
                          const VkrBakeryJson *result, const char *code,
                          const char *message) {
  Arena *arena = agent->arena;
  String8 body = {0};
  if (result) {
    if (!vkr_bakery_json_write(arena, result, VKR_BAKERY_JSON_COMPACT, &body)) {
      code = "VKR-AGENT-0008";
      message = "The result could not be serialized.";
      result = NULL;
    }
  }
  String8 text = {0};
  if (!result) {
    VkrBakeryJson *error = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, error, "code",
                        vkr_bakery_json_cstr(arena, code));
    vkr_bakery_json_set(arena, error, "message",
                        vkr_bakery_json_cstr(arena, message ? message : ""));
    (void)vkr_bakery_json_write(arena, error, VKR_BAKERY_JSON_COMPACT, &body);
  }
  const uint64_t capacity = body.length + id.length + 64u;
  char *line = arena_alloc(arena, capacity, ARENA_MEMORY_TAG_STRING);
  if (!line) {
    return;
  }
  if (!id.length) {
    id = string8_lit("null");
  }
  const int length = snprintf(
      line, capacity, "{\"v\":1,\"id\":%.*s,\"ok\":%s,\"%s\":%.*s}",
      (int)id.length, (const char *)id.str, result ? "true" : "false",
      result ? "result" : "error", (int)body.length, (const char *)body.str);
  if (length <= 0 || (uint64_t)length >= capacity) {
    return;
  }
  text = (String8){.str = (uint8_t *)line, .length = (uint64_t)length};
  agent_send(agent, slot, generation, text);
}

// =============================================================================
// Requests
// =============================================================================

static void agent_finish(VkrEditorAgent *agent) {
  free(agent->current.line);
  agent->current = (AgentQueued){0};
  agent->current_id = (String8){0};
  agent->active = false_v;
  arena_clear(agent->arena, ARENA_MEMORY_TAG_STRUCT);
}

/* Parses the next queued line and starts its operation. A malformed line
   answers at once. */
static void agent_begin(VkrEditorAgent *agent) {
  agent->current = agent->queue[agent->queue_head];
  agent->queue_head = (agent->queue_head + 1u) % AGENT_QUEUE_MAX;
  agent->queue_count--;
  agent->active = true_v;
  arena_clear(agent->arena, ARENA_MEMORY_TAG_STRUCT);
  VkrBakeryJsonError parse_error = {0};
  const VkrBakeryJson *request =
      vkr_bakery_json_parse(agent->arena, (const uint8_t *)agent->current.line,
                            agent->current.length, 64u, &parse_error);
  const VkrBakeryJson *id = request ? vkr_bakery_json_get(request, "id") : NULL;
  if (id &&
      (id->type == VKR_BAKERY_JSON_INT || id->type == VKR_BAKERY_JSON_FLOAT ||
       id->type == VKR_BAKERY_JSON_STRING)) {
    (void)vkr_bakery_json_write(agent->arena, id, VKR_BAKERY_JSON_COMPACT,
                                &agent->current_id);
  }
  String8 op = {0};
  int64_t version = 0;
  const VkrBakeryJson *args =
      request ? vkr_bakery_json_get(request, "args") : NULL;
  if (!request || request->type != VKR_BAKERY_JSON_OBJECT ||
      !vkr_bakery_json_get_int(request, "v", &version) || version != 1 ||
      !vkr_bakery_json_get_string(request, "op", &op) || !op.length ||
      (args && args->type != VKR_BAKERY_JSON_OBJECT)) {
    char message[160];
    snprintf(message, sizeof(message),
             request ? "A request needs \"v\":1, a string \"op\" and an "
                       "object \"args\"."
                     : "Malformed JSON at line %u, column %u: %s",
             parse_error.line, parse_error.column, parse_error.message);
    agent_respond(agent, agent->current.client, agent->current.generation,
                  agent->current_id, NULL, "VKR-AGENT-0001", message);
    agent_finish(agent);
    return;
  }
  agent->call = (VkrEditorOpCall){
      .arena = agent->arena,
      .op = op,
      .args = args ? args : vkr_bakery_json_object(agent->arena),
  };
}

static void agent_advance(VkrEditorAgent *agent, VkrEditorUi *editor,
                          const VkrSampleUiFrame *frame) {
  if (!agent->active) {
    if (!agent->queue_count) {
      return;
    }
    agent_begin(agent);
    if (!agent->active) {
      return;
    }
  }
  if (vkr_editor_ops_run(agent->ops, editor, frame, &agent->call) ==
      VKR_EDITOR_OP_WAIT) {
    return;
  }
  agent_respond(agent, agent->current.client, agent->current.generation,
                agent->current_id, agent->call.result,
                agent->call.error_code ? agent->call.error_code
                                       : "VKR-AGENT-0005",
                agent->call.error);
  agent_finish(agent);
}

static bool8_t agent_enqueue(VkrEditorAgent *agent, uint32_t client,
                             uint32_t generation, const uint8_t *line,
                             uint64_t length) {
  if (agent->queue_count == AGENT_QUEUE_MAX) {
    return false_v;
  }
  char *copy = malloc(length + 1u);
  if (!copy) {
    return false_v;
  }
  MemCopy(copy, line, length);
  copy[length] = '\0';
  const uint32_t slot =
      (agent->queue_head + agent->queue_count) % AGENT_QUEUE_MAX;
  agent->queue[slot] = (AgentQueued){.client = client,
                                     .generation = generation,
                                     .line = copy,
                                     .length = length};
  agent->queue_count++;
  return true_v;
}

bool8_t vkr_editor_agent_submit(VkrEditorAgent *agent, const char *line) {
  return agent && line &&
         agent_enqueue(agent, AGENT_CLIENT_SELF, 0u, (const uint8_t *)line,
                       strlen(line));
}

// =============================================================================
// Socket
// =============================================================================

bool8_t vkr_editor_agent_directory(char *out, uint64_t capacity) {
  char temp[512];
  if (!vkr_platform_user_directory(VKR_PLATFORM_USER_TEMP, temp,
                                   sizeof(temp))) {
    return false_v;
  }
  const int written = snprintf(out, capacity, "%s/vkr", temp);
  if (written <= 0 || (uint64_t)written >= capacity) {
    return false_v;
  }
#if defined(_WIN32)
  /* The per-user temporary directory already admits only its user. */
  const FilePath path = {
      .path = string8_create_from_cstr((const uint8_t *)out, strlen(out)),
      .type = FILE_PATH_TYPE_ABSOLUTE};
  return file_create_directory(&path);
#else
  if (mkdir(out, 0700) != 0 && errno != EEXIST) {
    return false_v;
  }
  struct stat info;
  return lstat(out, &info) == 0 && S_ISDIR(info.st_mode) &&
         info.st_uid == getuid() && (info.st_mode & 0077) == 0;
#endif
}

#if !defined(_WIN32)

static bool8_t agent_socket_live(const char *path) {
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return false_v;
  }
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
  const bool8_t live =
      connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0;
  close(fd);
  return live;
}

static bool8_t agent_listen(VkrEditorAgent *agent, const char *requested) {
  char directory[200];
  if (requested && requested[0]) {
    snprintf(agent->socket_path, sizeof(agent->socket_path), "%s", requested);
  } else {
    if (!vkr_editor_agent_directory(directory, sizeof(directory))) {
      snprintf(agent->status, sizeof(agent->status),
               "Agent channel off: no private directory for its socket.");
      return false_v;
    }
    snprintf(agent->socket_path, sizeof(agent->socket_path),
             "%s/editor-%u.sock", directory, (unsigned)getuid());
    /* Another editor already serves the default path. */
    if (agent_socket_live(agent->socket_path)) {
      snprintf(agent->socket_path, sizeof(agent->socket_path),
               "%s/editor-%u-%d.sock", directory, (unsigned)getuid(),
               (int)getpid());
    }
  }
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  if (strlen(agent->socket_path) >= sizeof(address.sun_path)) {
    snprintf(agent->status, sizeof(agent->status),
             "Agent channel off: the socket path is too long.");
    return false_v;
  }
  if (agent_socket_live(agent->socket_path)) {
    snprintf(agent->status, sizeof(agent->status),
             "Agent channel off: another editor listens on %s.",
             agent->socket_path);
    return false_v;
  }
  /* A socket file nobody answers is stale. */
  (void)unlink(agent->socket_path);
  snprintf(address.sun_path, sizeof(address.sun_path), "%s",
           agent->socket_path);
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    snprintf(agent->status, sizeof(agent->status),
             "Agent channel off: no socket (%s).", strerror(errno));
    return false_v;
  }
  /* Owner-only from creation: bind under a restrictive umask. */
  const mode_t previous = umask(0177);
  const int bound = bind(fd, (struct sockaddr *)&address, sizeof(address));
  umask(previous);
  if (bound != 0 || chmod(agent->socket_path, 0600) != 0 ||
      listen(fd, (int)AGENT_CLIENT_MAX) != 0) {
    snprintf(agent->status, sizeof(agent->status),
             "Agent channel off: cannot listen on %s (%s).", agent->socket_path,
             strerror(errno));
    close(fd);
    return false_v;
  }
  (void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  agent->listen_fd = fd;
  snprintf(agent->status, sizeof(agent->status), "Listening on %s",
           agent->socket_path);
  return true_v;
}

static void agent_accept(VkrEditorAgent *agent) {
  for (;;) {
    const int fd = accept(agent->listen_fd, NULL, NULL);
    if (fd < 0) {
      return;
    }
    AgentClient *slot = NULL;
    for (uint32_t i = 0; i < AGENT_CLIENT_MAX; ++i) {
      if (agent->clients[i].fd < 0) {
        slot = &agent->clients[i];
        break;
      }
    }
    if (!slot) {
      close(fd);
      continue;
    }
#if defined(SO_NOSIGPIPE)
    const int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    (void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    slot->fd = fd;
    slot->generation++;
    slot->in_length = 0u;
    slot->out_length = 0u;
  }
}

/* Reads what arrived and queues each complete line. */
static void agent_read(VkrEditorAgent *agent, uint32_t index) {
  AgentClient *client = &agent->clients[index];
  for (;;) {
    if (!agent_reserve(&client->in, &client->in_capacity,
                       client->in_length + 65536u)) {
      agent_client_close(client);
      return;
    }
    const ssize_t got = recv(client->fd, client->in + client->in_length,
                             client->in_capacity - client->in_length, 0);
    if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                     errno != EINTR)) {
      agent_client_close(client);
      return;
    }
    if (got < 0) {
      break;
    }
    client->in_length += (uint64_t)got;
  }
  uint64_t start = 0u;
  for (uint64_t i = 0u; i < client->in_length; ++i) {
    if (client->in[i] != '\n') {
      continue;
    }
    uint64_t end = i;
    if (end > start && client->in[end - 1u] == '\r') {
      end--;
    }
    if (end > start && !agent_enqueue(agent, index, client->generation,
                                      client->in + start, end - start)) {
      const String8 busy = string8_lit(
          "{\"v\":1,\"id\":null,\"ok\":false,\"error\":{\"code\":"
          "\"VKR-AGENT-0008\",\"message\":\"The request queue is full.\"}}");
      agent_send(agent, index, client->generation, busy);
    }
    start = i + 1u;
  }
  if (start) {
    MemCopy(client->in, client->in + start, client->in_length - start);
    client->in_length -= start;
  }
  if (client->in_length > AGENT_LINE_MAX) {
    agent_client_close(client);
  }
}

static void agent_write(AgentClient *client) {
  uint64_t sent_total = 0u;
  while (sent_total < client->out_length) {
#if defined(MSG_NOSIGNAL)
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    const ssize_t sent = send(client->fd, client->out + sent_total,
                              client->out_length - sent_total, flags);
    if (sent < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        break;
      }
      agent_client_close(client);
      return;
    }
    sent_total += (uint64_t)sent;
  }
  if (sent_total) {
    MemCopy(client->out, client->out + sent_total,
            client->out_length - sent_total);
    client->out_length -= sent_total;
  }
}

#endif

// =============================================================================
// Lifetime
// =============================================================================

VkrEditorAgent *vkr_editor_agent_create(VkrAllocator *allocator,
                                        const char *socket, bool8_t enabled) {
  VkrEditorAgent *agent = calloc(1u, sizeof(*agent));
  if (!agent) {
    return NULL;
  }
  agent->allocator = allocator;
  agent->listen_fd = -1;
  for (uint32_t i = 0; i < AGENT_CLIENT_MAX; ++i) {
    agent->clients[i].fd = -1;
  }
  agent->arena = arena_create(AGENT_ARENA_RESERVE, KB(64));
  agent->ops = vkr_editor_ops_create(allocator);
  if (!agent->arena || !agent->ops) {
    vkr_editor_agent_destroy(agent);
    return NULL;
  }
  if (!socket || !socket[0]) {
    socket = getenv("VKR_EDITOR_AGENT_SOCKET");
  }
#if defined(_WIN32)
  (void)socket;
  snprintf(agent->status, sizeof(agent->status),
           "Agent channel off: Windows support is pending.");
#else
  if (!enabled) {
    snprintf(agent->status, sizeof(agent->status),
             "Agent channel off (--no-agent-socket).");
  } else if (agent_listen(agent, socket)) {
    log_info("Agent channel: %s", agent->status);
  } else {
    log_warn("%s", agent->status);
  }
#endif
  return agent;
}

void vkr_editor_agent_destroy(VkrEditorAgent *agent) {
  if (!agent) {
    return;
  }
  for (uint32_t i = 0; i < AGENT_CLIENT_MAX; ++i) {
    agent_client_close(&agent->clients[i]);
    free(agent->clients[i].in);
    free(agent->clients[i].out);
  }
#if !defined(_WIN32)
  if (agent->listen_fd >= 0) {
    close(agent->listen_fd);
    (void)unlink(agent->socket_path);
  }
#endif
  while (agent->queue_count) {
    free(agent->queue[agent->queue_head].line);
    agent->queue_head = (agent->queue_head + 1u) % AGENT_QUEUE_MAX;
    agent->queue_count--;
  }
  free(agent->current.line);
  vkr_editor_ops_destroy(agent->ops);
  if (agent->arena) {
    arena_destroy(agent->arena);
  }
  free(agent);
}

void vkr_editor_agent_update(VkrEditorAgent *agent, VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame) {
  if (!agent) {
    return;
  }
  vkr_editor_ops_update(agent->ops, frame);
#if !defined(_WIN32)
  if (agent->listen_fd >= 0) {
    agent_accept(agent);
    for (uint32_t i = 0; i < AGENT_CLIENT_MAX; ++i) {
      if (agent->clients[i].fd >= 0) {
        agent_read(agent, i);
      }
    }
  }
#endif
  agent_advance(agent, editor, frame);
#if !defined(_WIN32)
  for (uint32_t i = 0; i < AGENT_CLIENT_MAX; ++i) {
    if (agent->clients[i].fd >= 0 && agent->clients[i].out_length) {
      agent_write(&agent->clients[i]);
    }
  }
#endif
}

bool8_t vkr_editor_agent_busy(const VkrEditorAgent *agent) {
  if (!agent) {
    return false_v;
  }
  for (uint32_t i = 0; i < AGENT_CLIENT_MAX; ++i) {
    if (agent->clients[i].fd >= 0) {
      return true_v;
    }
  }
  return agent->active || agent->queue_count > 0u;
}

const char *vkr_editor_agent_status(const VkrEditorAgent *agent) {
  return agent ? agent->status : "Agent channel off.";
}

struct VkrEditorOps *vkr_editor_agent_ops(VkrEditorAgent *agent) {
  return agent ? agent->ops : NULL;
}
