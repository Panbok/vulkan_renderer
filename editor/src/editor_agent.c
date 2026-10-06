#include "editor_agent.h"

#include "editor_internal.h"
#include "editor_ops.h"

#include "core/logger.h"
#include "platform/vkr_local_socket.h"
#include "platform/vkr_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Clients a listener serves at once, the longest request line and the
 * requests that may wait behind the active one. */
#define AGENT_CLIENT_MAX 4u
#define AGENT_LINE_MAX MB(1)
#define AGENT_QUEUE_MAX 64u
#define AGENT_ARENA_RESERVE MB(64)
/* A client slot for requests the editor submits itself. */
#define AGENT_CLIENT_SELF UINT32_MAX

typedef struct AgentClient {
  VkrLocalSocket socket;
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
  VkrLocalSocket listener;
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
  vkr_local_socket_close(client->socket);
  client->socket = VKR_LOCAL_SOCKET_INVALID;
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
  if (client->socket == VKR_LOCAL_SOCKET_INVALID ||
      client->generation != generation) {
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
  return vkr_local_socket_user_directory(out, capacity);
}

static void agent_listen_failed(VkrEditorAgent *agent,
                                VkrLocalSocketListenStatus status) {
  if (status == VKR_LOCAL_SOCKET_LISTEN_TOO_LONG) {
    snprintf(agent->status, sizeof(agent->status),
             "Agent channel off: the socket path is too long.");
  } else if (status == VKR_LOCAL_SOCKET_LISTEN_IN_USE) {
    snprintf(agent->status, sizeof(agent->status),
             "Agent channel off: another editor listens on %s.",
             agent->socket_path);
  } else {
    char error[200];
    vkr_local_socket_error_text(error, sizeof(error));
    snprintf(agent->status, sizeof(agent->status),
             "Agent channel off: cannot listen on %s (%s).", agent->socket_path,
             error);
  }
}

static bool8_t agent_listen(VkrEditorAgent *agent, const char *requested) {
  const bool8_t explicit_path = requested && requested[0];
  if (explicit_path) {
    snprintf(agent->socket_path, sizeof(agent->socket_path), "%s", requested);
  } else {
    char name[64];
    snprintf(name, sizeof(name), "editor-%u.sock", vkr_local_socket_user_id());
    if (!vkr_local_socket_user_path(name, agent->socket_path,
                                    sizeof(agent->socket_path))) {
      snprintf(agent->status, sizeof(agent->status),
               "Agent channel off: no private directory for its socket.");
      return false_v;
    }
  }
  VkrLocalSocketListenStatus status = vkr_local_socket_listen(
      agent->socket_path, (int32_t)AGENT_CLIENT_MAX, &agent->listener);
  if (status == VKR_LOCAL_SOCKET_LISTEN_IN_USE && !explicit_path) {
    /* Another editor already serves the default path. */
    char name[64];
    snprintf(name, sizeof(name), "editor-%u-%u.sock",
             vkr_local_socket_user_id(), vkr_platform_get_process_id());
    status = vkr_local_socket_user_path(name, agent->socket_path,
                                        sizeof(agent->socket_path))
                 ? vkr_local_socket_listen(agent->socket_path,
                                           (int32_t)AGENT_CLIENT_MAX,
                                           &agent->listener)
                 : VKR_LOCAL_SOCKET_LISTEN_TOO_LONG;
  }
  if (status != VKR_LOCAL_SOCKET_LISTEN_OK) {
    agent_listen_failed(agent, status);
    return false_v;
  }
  (void)vkr_local_socket_set_nonblocking(agent->listener);
  snprintf(agent->status, sizeof(agent->status), "Listening on %s",
           agent->socket_path);
  return true_v;
}

static void agent_accept(VkrEditorAgent *agent) {
  for (;;) {
    VkrLocalSocket accepted = VKR_LOCAL_SOCKET_INVALID;
    if (vkr_local_socket_accept(agent->listener, &accepted) !=
        VKR_LOCAL_SOCKET_OK) {
      return;
    }
    AgentClient *slot = NULL;
    for (uint32_t i = 0; i < AGENT_CLIENT_MAX; ++i) {
      if (agent->clients[i].socket == VKR_LOCAL_SOCKET_INVALID) {
        slot = &agent->clients[i];
        break;
      }
    }
    if (!slot) {
      vkr_local_socket_close(accepted);
      continue;
    }
    (void)vkr_local_socket_set_nonblocking(accepted);
    slot->socket = accepted;
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
    uint64_t got = 0u;
    const VkrLocalSocketStatus status =
        vkr_local_socket_recv(client->socket, client->in + client->in_length,
                              client->in_capacity - client->in_length, &got);
    if (status == VKR_LOCAL_SOCKET_WOULD_BLOCK) {
      break;
    }
    if (status != VKR_LOCAL_SOCKET_OK) {
      agent_client_close(client);
      return;
    }
    client->in_length += got;
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
    uint64_t sent = 0u;
    const VkrLocalSocketStatus status =
        vkr_local_socket_send(client->socket, client->out + sent_total,
                              client->out_length - sent_total, &sent);
    if (status == VKR_LOCAL_SOCKET_WOULD_BLOCK) {
      break;
    }
    if (status != VKR_LOCAL_SOCKET_OK) {
      agent_client_close(client);
      return;
    }
    sent_total += sent;
  }
  if (sent_total) {
    MemCopy(client->out, client->out + sent_total,
            client->out_length - sent_total);
    client->out_length -= sent_total;
  }
}

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
  agent->listener = VKR_LOCAL_SOCKET_INVALID;
  for (uint32_t i = 0; i < AGENT_CLIENT_MAX; ++i) {
    agent->clients[i].socket = VKR_LOCAL_SOCKET_INVALID;
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
  if (!enabled) {
    snprintf(agent->status, sizeof(agent->status),
             "Agent channel off (--no-agent-socket).");
  } else if (agent_listen(agent, socket)) {
    log_info("Agent channel: %s", agent->status);
  } else {
    log_warn("%s", agent->status);
  }
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
  if (agent->listener != VKR_LOCAL_SOCKET_INVALID) {
    vkr_local_socket_close(agent->listener);
    (void)vkr_local_socket_remove_path(agent->socket_path);
  }
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
  if (agent->listener != VKR_LOCAL_SOCKET_INVALID) {
    agent_accept(agent);
    for (uint32_t i = 0; i < AGENT_CLIENT_MAX; ++i) {
      if (agent->clients[i].socket != VKR_LOCAL_SOCKET_INVALID) {
        agent_read(agent, i);
      }
    }
  }
  agent_advance(agent, editor, frame);
  for (uint32_t i = 0; i < AGENT_CLIENT_MAX; ++i) {
    if (agent->clients[i].socket != VKR_LOCAL_SOCKET_INVALID &&
        agent->clients[i].out_length) {
      agent_write(&agent->clients[i]);
    }
  }
}

bool8_t vkr_editor_agent_busy(const VkrEditorAgent *agent) {
  if (!agent) {
    return false_v;
  }
  for (uint32_t i = 0; i < AGENT_CLIENT_MAX; ++i) {
    if (agent->clients[i].socket != VKR_LOCAL_SOCKET_INVALID) {
      return true_v;
    }
  }
  return agent->active || agent->queue_count > 0u;
}

bool8_t vkr_editor_agent_self_pending(const VkrEditorAgent *agent) {
  if (!agent) {
    return false_v;
  }
  if (agent->active && agent->current.client == AGENT_CLIENT_SELF) {
    return true_v;
  }
  for (uint32_t i = 0; i < agent->queue_count; ++i) {
    const uint32_t slot = (agent->queue_head + i) % AGENT_QUEUE_MAX;
    if (agent->queue[slot].client == AGENT_CLIENT_SELF) {
      return true_v;
    }
  }
  return false_v;
}

const char *vkr_editor_agent_status(const VkrEditorAgent *agent) {
  return agent ? agent->status : "Agent channel off.";
}

struct VkrEditorOps *vkr_editor_agent_ops(VkrEditorAgent *agent) {
  return agent ? agent->ops : NULL;
}
