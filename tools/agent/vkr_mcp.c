/* vkr_mcp: an MCP server over stdio for the VKR editor's agent channel
 * (ADR-084).
 *
 * It speaks MCP revision 2026-07-28 only: one JSON-RPC message per line,
 * no initialize handshake, and the protocol version in each request's
 * `_meta`. Every editor operation from `ops.list` becomes the tool
 * `vkr_<operation with dots as underscores>`; a call forwards to the editor
 * socket and returns the operation's JSON as structured content. A capture
 * also returns its PNG as image content. `--agent <name>` names every
 * request's author unless a call names its own. The change feed is the
 * resource `vkr://editor/changes`. `subscriptions/listen` pushes its updates
 * and tool-list changes from a listener thread with its own editor
 * connection, while the main thread answers requests in order. Logs go to
 * stderr. */

#include "containers/str.h"
#include "core/vkr_threads.h"
#include "defines.h"
#include "filesystem/filesystem.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_local_socket.h"
#include "vkr_bakery_json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#define MCP_VERSION "2026-07-28"
#define MCP_SERVER_NAME "vkr-editor"
#define MCP_SERVER_VERSION "1.0.0"
/* JSON-RPC and MCP error codes. */
#define MCP_PARSE_ERROR -32700
#define MCP_INVALID_REQUEST -32600
#define MCP_METHOD_NOT_FOUND -32601
#define MCP_INVALID_PARAMS -32602
#define MCP_INTERNAL_ERROR -32603
#define MCP_UNSUPPORTED_VERSION -32022
/* Longest editor response line; captures travel as file paths. */
#define MCP_LINE_MAX MB(16)
/* The change feed as a resource, and the form that reads after a
   sequence. */
#define MCP_FEED_URI "vkr://editor/changes"
#define MCP_FEED_TEMPLATE "vkr://editor/changes{?after}"
/* Open subscriptions, and the longest subscription id as JSON text. */
#define MCP_SUBSCRIPTION_MAX 16u
#define MCP_SUBSCRIPTION_ID_MAX 128u
/* How long the listener's feed read waits in the editor, in seconds. */
#define MCP_LISTEN_WAIT_SECONDS 30
/* The least time between two feed notifications, in milliseconds: a
   designer's drag moves a journal revision every frame, so changes gather
   for this long and reach a client as one notification. */
#define MCP_NOTIFY_GAP_MS 500
/* The pause between attempts to reach an editor that is not running. */
#define MCP_RECONNECT_MS 1000

static const char *s_instructions =
    "Tools drive the open VKR editor. Units are meters with Y up; rotations "
    "are degrees XYZ. Start with vkr_editor_status and vkr_scene_describe "
    "(page it with root or region). Plan a layout as data, then send it as "
    "one vkr_batch: its writes apply as one undo step, $k names the entity "
    "operation k created, and dry_run checks it first. Each write becomes a "
    "pending change the designer accepts or rejects, so pass 'agent' (your "
    "name) and a 'label', and report what you changed. Working beside "
    "other agents, claim your region first (vkr_claims_set); writes into "
    "another agent's claim are refused. Read vkr_changes_feed with the last "
    "'next' you saw to learn what others changed; 'wait' answers once "
    "something changes. A client with subscriptions/listen can subscribe "
    "to the resource " MCP_FEED_URI " instead: a notification arrives, at "
    "most twice a second, when the feed or an edit moves it, and you then "
    "read the feed from your 'next'. Prefer blockout shapes, "
    "snapping and vkr_entity_place (on, inside, against) to computed "
    "corners. Pass settle true on a write "
    "to get each entity's world bounds and build status once it rebuilt. "
    "Collision and geometry reads (raycast, reachable, bounds, level_lint, "
    "level_map, capture) wait for rebuilds by default, so they never see a "
    "stale scene. Verify with numbers before pictures: vkr_query_raycast and "
    "vkr_terrain_sample for heights and openings, vkr_query_reachable for "
    "routes, vkr_level_map for a text floor plan (rows run +z down the "
    "page, +x across, as a top capture shows), vkr_level_lint in regions "
    "of 76 m or less for full detail. Use vkr_view_capture (view top with "
    "grid_labels, or eye and target) to judge the look: pass max_width (768 "
    "is plenty), put several views in one sheet with 'views', and pass "
    "'marks' (world points, with labels) to see where they land and "
    "whether walls hide them. Undo "
    "takes only "
    "your own batches; reject your change with vkr_changes_reject instead "
    "of undoing other work.";

/* One connection to the editor's agent channel. */
typedef struct McpEditor {
  const char *socket_path;
  /* The author every request names unless its arguments name one; NULL
     or empty for none. */
  const char *agent;
  VkrLocalSocket socket;
  uint64_t next_id;
  /* Counts connections made, so a cache knows the editor may have
     restarted since it was filled. */
  uint64_t connections;
  /* Bytes received after the last complete editor line. */
  char *pending;
  uint64_t pending_length;
  uint64_t pending_capacity;
} McpEditor;

/* An open `subscriptions/listen` request and what it agreed to receive. */
typedef struct McpSubscription {
  /* The request's id as compact JSON; every notification carries it. */
  char id[MCP_SUBSCRIPTION_ID_MAX];
  bool8_t tools;
  bool8_t changes;
} McpSubscription;

/* A thread on its own editor connection that waits on the change feed and
   notifies the open subscriptions. The output mutex guards `quit` and the
   subscriptions. It starts with the first subscription and stops when
   stdin closes. */
typedef struct McpListener {
  bool8_t started;
  bool8_t quit;
  VkrThread thread;
  /* The main thread writes a byte to wake[0] when the listener must look
     at the subscriptions again; the listener polls wake[1]. */
  VkrLocalSocket wake[2];
  McpSubscription subscriptions[MCP_SUBSCRIPTION_MAX];
  uint32_t subscription_count;
  /* Owned by the listener thread. */
  McpEditor editor;
  Arena *arena;
} McpListener;

typedef struct McpState {
  char socket_path[512];
  /* The author every request names unless its arguments name one. */
  char agent[32];
  /* The main thread's connection, for tool calls and resource reads. */
  McpEditor editor;
  /* The editor's `ops.list` result as JSON text and the connection it came
     from, so a tool call costs the editor one request instead of two. */
  char *ops_text;
  uint64_t ops_length;
  uint64_t ops_connection;
  /* Backs the thread and the mutex for the process's lifetime. */
  Arena *sync_arena;
  VkrAllocator sync_allocator;
  McpListener listener;
} McpState;

/* Guards stdout, so the main thread's responses and the listener's
   notifications each reach the client as whole lines, and the listener's
   subscriptions. */
static VkrMutex s_output_mutex;

static void mcp_log(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  fprintf(stderr, "vkr_mcp: ");
  vfprintf(stderr, format, arguments);
  fprintf(stderr, "\n");
  va_end(arguments);
}

// =============================================================================
// Output
// =============================================================================

/* Writes one message line; the caller holds the output mutex. */
static void mcp_write_line_locked(const uint8_t *text, uint64_t length) {
  fwrite(text, 1, length, stdout);
  fputc('\n', stdout);
  fflush(stdout);
}

static void mcp_write(Arena *arena, const VkrBakeryJson *message) {
  String8 text = {0};
  if (!vkr_bakery_json_write(arena, message, VKR_BAKERY_JSON_COMPACT, &text)) {
    mcp_log("a response could not be serialized");
    return;
  }
  vkr_mutex_lock(s_output_mutex);
  mcp_write_line_locked(text.str, text.length);
  vkr_mutex_unlock(s_output_mutex);
}

static VkrBakeryJson *mcp_response(Arena *arena, const VkrBakeryJson *id) {
  VkrBakeryJson *message = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, message, "jsonrpc",
                      vkr_bakery_json_cstr(arena, "2.0"));
  vkr_bakery_json_set(arena, message, "id",
                      id ? vkr_bakery_json_clone(arena, id)
                         : vkr_bakery_json_null(arena));
  return message;
}

static void mcp_error(Arena *arena, const VkrBakeryJson *id, int64_t code,
                      const char *text, VkrBakeryJson *data) {
  VkrBakeryJson *message = mcp_response(arena, id);
  VkrBakeryJson *error = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, error, "code", vkr_bakery_json_int(arena, code));
  vkr_bakery_json_set(arena, error, "message",
                      vkr_bakery_json_cstr(arena, text));
  if (data) {
    vkr_bakery_json_set(arena, error, "data", data);
  }
  vkr_bakery_json_set(arena, message, "error", error);
  mcp_write(arena, message);
}

static void mcp_unsupported(Arena *arena, const VkrBakeryJson *id,
                            const VkrBakeryJson *requested) {
  VkrBakeryJson *data = vkr_bakery_json_object(arena);
  VkrBakeryJson *supported = vkr_bakery_json_array(arena);
  vkr_bakery_json_append(supported, vkr_bakery_json_cstr(arena, MCP_VERSION));
  vkr_bakery_json_set(arena, data, "supported", supported);
  vkr_bakery_json_set(arena, data, "requested",
                      requested ? vkr_bakery_json_clone(arena, requested)
                                : vkr_bakery_json_null(arena));
  mcp_error(arena, id, MCP_UNSUPPORTED_VERSION,
            "Unsupported protocol version; this server speaks " MCP_VERSION
            " only",
            data);
}

/* A result object with the fields every result carries. */
static VkrBakeryJson *mcp_result(Arena *arena) {
  VkrBakeryJson *result = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, result, "resultType",
                      vkr_bakery_json_cstr(arena, "complete"));
  VkrBakeryJson *meta = vkr_bakery_json_object(arena);
  VkrBakeryJson *info = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, info, "name",
                      vkr_bakery_json_cstr(arena, MCP_SERVER_NAME));
  vkr_bakery_json_set(arena, info, "version",
                      vkr_bakery_json_cstr(arena, MCP_SERVER_VERSION));
  vkr_bakery_json_set(arena, meta, "io.modelcontextprotocol/serverInfo", info);
  vkr_bakery_json_set(arena, result, "_meta", meta);
  return result;
}

/* Sets the caching hints a cacheable result carries. */
static void mcp_cache(Arena *arena, VkrBakeryJson *result, int64_t ttl_ms) {
  vkr_bakery_json_set(arena, result, "ttlMs",
                      vkr_bakery_json_int(arena, ttl_ms));
  vkr_bakery_json_set(arena, result, "cacheScope",
                      vkr_bakery_json_cstr(arena, "private"));
}

static void mcp_send_result(Arena *arena, const VkrBakeryJson *id,
                            VkrBakeryJson *result) {
  VkrBakeryJson *message = mcp_response(arena, id);
  vkr_bakery_json_set(arena, message, "result", result);
  mcp_write(arena, message);
}

// =============================================================================
// Editor socket
// =============================================================================

static void mcp_editor_close(McpEditor *editor) {
  vkr_local_socket_close(editor->socket);
  editor->socket = VKR_LOCAL_SOCKET_INVALID;
  editor->pending_length = 0u;
}

static bool8_t mcp_editor_connect(McpEditor *editor) {
  if (editor->socket != VKR_LOCAL_SOCKET_INVALID) {
    return true_v;
  }
  if (!vkr_local_socket_connect(editor->socket_path, &editor->socket)) {
    return false_v;
  }
  editor->pending_length = 0u;
  editor->connections++;
  return true_v;
}

typedef enum McpRead {
  MCP_READ_LINE = 0,
  MCP_READ_FAILED,
  /* The wake socket became readable before a line arrived. */
  MCP_READ_WOKEN,
} McpRead;

/* Reads the next complete line from the editor into the arena. With a
   valid `wake` socket it also returns once that socket is readable. */
static McpRead mcp_editor_read_line(McpEditor *editor, Arena *arena,
                                    VkrLocalSocket wake, String8 *out) {
  for (;;) {
    for (uint64_t i = 0; i < editor->pending_length; ++i) {
      if (editor->pending[i] != '\n') {
        continue;
      }
      char *line = arena_alloc(arena, i + 1u, ARENA_MEMORY_TAG_STRING);
      if (!line) {
        return MCP_READ_FAILED;
      }
      MemCopy(line, editor->pending, i);
      line[i] = '\0';
      MemCopy(editor->pending, editor->pending + i + 1u,
              editor->pending_length - i - 1u);
      editor->pending_length -= i + 1u;
      *out = (String8){.str = (uint8_t *)line, .length = i};
      return MCP_READ_LINE;
    }

    if (wake != VKR_LOCAL_SOCKET_INVALID) {
      VkrLocalSocketPoll entries[2] = {{.socket = editor->socket},
                                       {.socket = wake}};
      if (vkr_local_socket_poll(entries, 2u, -1) < 0) {
        return MCP_READ_FAILED;
      }
      if (entries[1].readable) {
        return MCP_READ_WOKEN;
      }
      if (!entries[0].readable) {
        continue;
      }
    }

    if (editor->pending_length + 65536u > editor->pending_capacity) {
      const uint64_t capacity =
          Max(editor->pending_capacity * 2u, (uint64_t)131072u);
      if (capacity > MCP_LINE_MAX) {
        return MCP_READ_FAILED;
      }
      char *grown = realloc(editor->pending, capacity);
      if (!grown) {
        return MCP_READ_FAILED;
      }
      editor->pending = grown;
      editor->pending_capacity = capacity;
    }
    uint64_t got = 0u;
    if (vkr_local_socket_recv(editor->socket,
                              editor->pending + editor->pending_length,
                              editor->pending_capacity - editor->pending_length,
                              &got) != VKR_LOCAL_SOCKET_OK) {
      return MCP_READ_FAILED;
    }
    editor->pending_length += got;
  }
}

/* The request line for `op` with `args`, as compact JSON under the next
   id. */
static bool8_t mcp_editor_request(McpEditor *editor, Arena *arena,
                                  const char *op, const VkrBakeryJson *args,
                                  String8 *out_text) {
  VkrBakeryJson *request = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, request, "v", vkr_bakery_json_int(arena, 1));
  vkr_bakery_json_set(arena, request, "id",
                      vkr_bakery_json_int(arena, (int64_t)++editor->next_id));
  vkr_bakery_json_set(arena, request, "op", vkr_bakery_json_cstr(arena, op));
  if (editor->agent && editor->agent[0]) {
    vkr_bakery_json_set(arena, request, "agent",
                        vkr_bakery_json_cstr(arena, editor->agent));
  }
  vkr_bakery_json_set(arena, request, "args",
                      args && args->type == VKR_BAKERY_JSON_OBJECT
                          ? vkr_bakery_json_clone(arena, args)
                          : vkr_bakery_json_object(arena));
  return vkr_bakery_json_write(arena, request, VKR_BAKERY_JSON_COMPACT,
                               out_text);
}

static bool8_t mcp_editor_send(McpEditor *editor, String8 text) {
  return vkr_local_socket_send_all(editor->socket, text.str, text.length) &&
         vkr_local_socket_send_all(editor->socket, "\n", 1u);
}

/* Runs one editor operation and returns its response object, or NULL with
   `error` set when the editor cannot be reached. */
static const VkrBakeryJson *mcp_editor_call(McpEditor *editor, Arena *arena,
                                            const char *op,
                                            const VkrBakeryJson *args,
                                            char *error, uint64_t capacity) {
  String8 text = {0};
  if (!mcp_editor_request(editor, arena, op, args, &text)) {
    snprintf(error, capacity, "The arguments could not be serialized");
    return NULL;
  }
  /* One reconnect covers an editor that restarted since the last call. */
  for (uint32_t attempt = 0; attempt < 2u; ++attempt) {
    if (!mcp_editor_connect(editor)) {
      snprintf(error, capacity,
               "The VKR editor is not running or has its agent channel off "
               "(no listener at %s). Start the editor and retry.",
               editor->socket_path);
      return NULL;
    }
    String8 line = {0};
    if (mcp_editor_send(editor, text) &&
        mcp_editor_read_line(editor, arena, VKR_LOCAL_SOCKET_INVALID, &line) ==
            MCP_READ_LINE) {
      const VkrBakeryJson *response =
          vkr_bakery_json_parse(arena, line.str, line.length, 64u, NULL);
      if (!response) {
        snprintf(error, capacity, "The editor sent malformed JSON");
      }
      return response;
    }
    mcp_editor_close(editor);
  }
  snprintf(error, capacity, "The editor closed the connection");
  return NULL;
}

/* The editor's error message from a failed response, or `fallback`. */
static void mcp_editor_error(const VkrBakeryJson *response,
                             const char *fallback, char *out,
                             uint64_t capacity) {
  const VkrBakeryJson *editor_error =
      response ? vkr_bakery_json_get(response, "error") : NULL;
  String8 message = {0};
  if (editor_error &&
      vkr_bakery_json_get_string(editor_error, "message", &message)) {
    snprintf(out, capacity, "%.*s", (int)message.length,
             (const char *)message.str);
  } else {
    snprintf(out, capacity, "%s", fallback);
  }
}

// =============================================================================
// Listener
// =============================================================================

typedef enum McpNotify {
  MCP_NOTIFY_TOOLS = 0,
  MCP_NOTIFY_CHANGES,
} McpNotify;

/* Sends one notification to each open subscription that asked for it. */
static void mcp_notify(McpListener *listener, McpNotify kind) {
  char line[512];
  vkr_mutex_lock(s_output_mutex);
  for (uint32_t i = 0; i < listener->subscription_count; ++i) {
    const McpSubscription *subscription = &listener->subscriptions[i];
    if (kind == MCP_NOTIFY_CHANGES ? !subscription->changes
                                   : !subscription->tools) {
      continue;
    }
    const int length = snprintf(
        line, sizeof(line),
        "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":{\"_meta\":{"
        "\"io.modelcontextprotocol/subscriptionId\":%s}%s}}",
        kind == MCP_NOTIFY_CHANGES ? "notifications/resources/updated"
                                   : "notifications/tools/list_changed",
        subscription->id,
        kind == MCP_NOTIFY_CHANGES ? ",\"uri\":\"" MCP_FEED_URI "\"" : "");
    if (length > 0 && (uint64_t)length < sizeof(line)) {
      mcp_write_line_locked((const uint8_t *)line, (uint64_t)length);
    }
  }
  vkr_mutex_unlock(s_output_mutex);
}

/* Consumes the main thread's pending wake bytes. */
static void mcp_listener_drain(McpListener *listener) {
  char bytes[64];
  uint64_t got = 0u;
  (void)vkr_local_socket_recv(listener->wake[1], bytes, sizeof(bytes), &got);
}

/* Waits up to `milliseconds` (negative without limit) or until the main
   thread wakes the listener. False when the wait itself failed. */
static bool8_t mcp_listener_sleep(McpListener *listener, int32_t milliseconds) {
  VkrLocalSocketPoll entry = {.socket = listener->wake[1]};
  const int32_t ready = vkr_local_socket_poll(&entry, 1u, milliseconds);
  if (ready > 0 && entry.readable) {
    mcp_listener_drain(listener);
  }
  return ready >= 0;
}

static void mcp_listener_wake(McpListener *listener) {
  const uint8_t byte = 1u;
  if (listener->started) {
    (void)vkr_local_socket_send_all(listener->wake[0], &byte, 1u);
  }
}

/* The change feed's newest sequence and the journal revisions, as
   `changes.feed` reports them. */
typedef struct McpFeedPosition {
  int64_t latest;
  int64_t scene;
  int64_t world;
} McpFeedPosition;

/* Reads the feed's position: at once before it is `known`, else once the
   editor has something newer than `seen` or the wait ends. A wake from the
   main thread interrupts it. */
static McpRead mcp_listener_read(McpListener *listener, bool8_t known,
                                 McpFeedPosition seen, McpFeedPosition *out,
                                 char *error, uint64_t capacity) {
  Arena *arena = listener->arena;
  McpEditor *editor = &listener->editor;
  VkrBakeryJson *args = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, args, "after",
                      vkr_bakery_json_int(arena, seen.latest));
  vkr_bakery_json_set(arena, args, "limit", vkr_bakery_json_int(arena, 1));
  if (known) {
    VkrBakeryJson *revisions = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, revisions, "scene",
                        vkr_bakery_json_int(arena, seen.scene));
    vkr_bakery_json_set(arena, revisions, "world",
                        vkr_bakery_json_int(arena, seen.world));
    vkr_bakery_json_set(arena, args, "revisions", revisions);
    vkr_bakery_json_set(arena, args, "wait",
                        vkr_bakery_json_int(arena, MCP_LISTEN_WAIT_SECONDS));
  }

  String8 text = {0};
  String8 line = {0};
  if (!mcp_editor_request(editor, arena, "changes.feed", args, &text) ||
      !mcp_editor_send(editor, text)) {
    snprintf(error, capacity, "the editor closed the connection");
    return MCP_READ_FAILED;
  }
  const McpRead read =
      mcp_editor_read_line(editor, arena, listener->wake[1], &line);
  if (read != MCP_READ_LINE) {
    snprintf(error, capacity, "the editor closed the connection");
    return read;
  }

  const VkrBakeryJson *response =
      vkr_bakery_json_parse(arena, line.str, line.length, 64u, NULL);
  const VkrBakeryJson *result =
      response ? vkr_bakery_json_get(response, "result") : NULL;
  const VkrBakeryJson *revisions =
      result ? vkr_bakery_json_get(result, "revisions") : NULL;
  if (!revisions || !vkr_bakery_json_get_int(result, "latest", &out->latest) ||
      !vkr_bakery_json_get_int(revisions, "scene", &out->scene) ||
      !vkr_bakery_json_get_int(revisions, "world", &out->world)) {
    mcp_editor_error(response, "the editor did not read its change feed", error,
                     capacity);
    return MCP_READ_FAILED;
  }
  return MCP_READ_LINE;
}

/* The listener thread. Each pass reads the change feed with a wait parked
   in the editor (ADR-084), so it costs the editor nothing between edits,
   and notifies when the feed or a journal revision moved. */
static void *mcp_listen(void *argument) {
  McpListener *listener = argument;
  McpEditor *editor = &listener->editor;
  /* The feed position the last read found; nothing before the first. */
  bool8_t known = false_v;
  McpFeedPosition seen = {0};
  /* The editor was out of reach since the last connection, so it may have
     started or restarted with other operations. */
  bool8_t lost = false_v;
  for (;;) {
    arena_clear(listener->arena, ARENA_MEMORY_TAG_STRUCT);
    vkr_mutex_lock(s_output_mutex);
    const bool8_t quit = listener->quit;
    const bool8_t active = listener->subscription_count > 0u;
    vkr_mutex_unlock(s_output_mutex);
    if (quit) {
      break;
    }
    if (!active) {
      /* Nothing to watch: give the editor its client back until a
         subscription opens. */
      mcp_editor_close(editor);
      if (!mcp_listener_sleep(listener, -1)) {
        break;
      }
      continue;
    }

    if (editor->socket == VKR_LOCAL_SOCKET_INVALID) {
      if (!mcp_editor_connect(editor)) {
        lost = true_v;
        if (!mcp_listener_sleep(listener, MCP_RECONNECT_MS)) {
          break;
        }
        continue;
      }
      if (lost) {
        mcp_notify(listener, MCP_NOTIFY_TOOLS);
        lost = false_v;
      }
    }

    McpFeedPosition now = {0};
    char error[256];
    const McpRead read =
        mcp_listener_read(listener, known, seen, &now, error, sizeof(error));
    if (read == MCP_READ_WOKEN) {
      /* The last subscription closed or stdin did: drop the parked read
         and look again. */
      mcp_listener_drain(listener);
      mcp_editor_close(editor);
      continue;
    }
    if (read == MCP_READ_FAILED) {
      /* A closed connection, a restarted editor or a refusal when it
         serves its most clients: reach it again shortly. */
      if (!lost) {
        mcp_log("listener: %s; retrying every %d ms", error, MCP_RECONNECT_MS);
      }
      mcp_editor_close(editor);
      lost = true_v;
      if (!mcp_listener_sleep(listener, MCP_RECONNECT_MS)) {
        break;
      }
      continue;
    }

    const bool8_t moved =
        known && (now.latest != seen.latest || now.scene != seen.scene ||
                  now.world != seen.world);
    known = true_v;
    seen = now;
    if (moved) {
      mcp_notify(listener, MCP_NOTIFY_CHANGES);
      if (!mcp_listener_sleep(listener, MCP_NOTIFY_GAP_MS)) {
        break;
      }
    }
  }
  mcp_editor_close(editor);
  return NULL;
}

/* Starts the listener the first time a subscription needs it. */
static bool8_t mcp_listener_start(McpState *state) {
  McpListener *listener = &state->listener;
  if (listener->started) {
    return true_v;
  }
  listener->editor = (McpEditor){
      .socket_path = state->socket_path,
      .socket = VKR_LOCAL_SOCKET_INVALID,
  };
  listener->arena = arena_create(MB(64), KB(64));
  if (!listener->arena) {
    return false_v;
  }
  if (!vkr_local_socket_pair(listener->wake)) {
    arena_destroy(listener->arena);
    listener->arena = NULL;
    return false_v;
  }
  if (!vkr_thread_create(&state->sync_allocator, &listener->thread, mcp_listen,
                         listener)) {
    vkr_local_socket_close(listener->wake[0]);
    vkr_local_socket_close(listener->wake[1]);
    arena_destroy(listener->arena);
    listener->arena = NULL;
    return false_v;
  }
  listener->started = true_v;
  return true_v;
}

/* Stops the listener once stdin closed; the client is gone, so its
   subscriptions end without a final response. */
static void mcp_listener_stop(McpState *state) {
  McpListener *listener = &state->listener;
  if (!listener->started) {
    return;
  }
  vkr_mutex_lock(s_output_mutex);
  listener->quit = true_v;
  vkr_mutex_unlock(s_output_mutex);
  mcp_listener_wake(listener);
  (void)vkr_thread_join(listener->thread);
  (void)vkr_thread_destroy(&state->sync_allocator, &listener->thread);
  vkr_local_socket_close(listener->wake[0]);
  vkr_local_socket_close(listener->wake[1]);
  free(listener->editor.pending);
  arena_destroy(listener->arena);
  listener->started = false_v;
}

// =============================================================================
// Methods
// =============================================================================

static void mcp_discover(Arena *arena, const VkrBakeryJson *id) {
  VkrBakeryJson *result = mcp_result(arena);
  VkrBakeryJson *versions = vkr_bakery_json_array(arena);
  vkr_bakery_json_append(versions, vkr_bakery_json_cstr(arena, MCP_VERSION));
  vkr_bakery_json_set(arena, result, "supportedVersions", versions);
  VkrBakeryJson *capabilities = vkr_bakery_json_object(arena);
  VkrBakeryJson *tools = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, tools, "listChanged",
                      vkr_bakery_json_bool(arena, true_v));
  vkr_bakery_json_set(arena, capabilities, "tools", tools);
  VkrBakeryJson *resources = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, resources, "subscribe",
                      vkr_bakery_json_bool(arena, true_v));
  vkr_bakery_json_set(arena, capabilities, "resources", resources);
  vkr_bakery_json_set(arena, result, "capabilities", capabilities);
  vkr_bakery_json_set(arena, result, "instructions",
                      vkr_bakery_json_cstr(arena, s_instructions));
  mcp_cache(arena, result, 3600000);
  mcp_send_result(arena, id, result);
}

/* `vkr_` and the operation name with dots as underscores. */
static void mcp_tool_name(String8 op, char *out, uint64_t capacity) {
  const int written =
      snprintf(out, capacity, "vkr_%.*s", (int)op.length, op.str);
  for (int i = 0; i < written && out[i]; ++i) {
    if (out[i] == '.') {
      out[i] = '_';
    }
  }
}

/* The editor's operations, from the copy kept for the connection or from
   `ops.list`. */
static const VkrBakeryJson *mcp_ops(McpState *state, Arena *arena, char *error,
                                    uint64_t capacity) {
  if (state->ops_text && state->editor.socket != VKR_LOCAL_SOCKET_INVALID &&
      state->ops_connection == state->editor.connections) {
    const VkrBakeryJson *kept = vkr_bakery_json_parse(
        arena, (const uint8_t *)state->ops_text, state->ops_length, 64u, NULL);
    const VkrBakeryJson *ops = kept ? vkr_bakery_json_get(kept, "ops") : NULL;
    if (ops && ops->type == VKR_BAKERY_JSON_ARRAY) {
      return ops;
    }
  }
  const VkrBakeryJson *response =
      mcp_editor_call(&state->editor, arena, "ops.list", NULL, error, capacity);
  bool8_t ok = false_v;
  const VkrBakeryJson *result =
      response ? vkr_bakery_json_get(response, "result") : NULL;
  const VkrBakeryJson *ops = result ? vkr_bakery_json_get(result, "ops") : NULL;
  if (!response) {
    return NULL;
  }
  if (!vkr_bakery_json_get_bool(response, "ok", &ok) || !ok || !ops ||
      ops->type != VKR_BAKERY_JSON_ARRAY) {
    /* The editor's own reason, as when it serves its most clients. */
    mcp_editor_error(response, "The editor did not list its operations", error,
                     capacity);
    return NULL;
  }
  String8 text = {0};
  if (vkr_bakery_json_write(arena, result, VKR_BAKERY_JSON_COMPACT, &text)) {
    char *copy = malloc(text.length);
    if (copy) {
      MemCopy(copy, text.str, text.length);
      free(state->ops_text);
      state->ops_text = copy;
      state->ops_length = text.length;
      state->ops_connection = state->editor.connections;
    }
  }
  return ops;
}

static void mcp_tools_list(McpState *state, Arena *arena,
                           const VkrBakeryJson *id) {
  char error[512];
  const VkrBakeryJson *ops = mcp_ops(state, arena, error, sizeof(error));
  if (!ops) {
    mcp_error(arena, id, MCP_INTERNAL_ERROR, error, NULL);
    return;
  }
  VkrBakeryJson *tools = vkr_bakery_json_array(arena);
  for (const VkrBakeryJson *op = ops->first; op; op = op->next) {
    String8 name = {0};
    String8 description = {0};
    const VkrBakeryJson *schema = vkr_bakery_json_get(op, "schema");
    if (!vkr_bakery_json_get_string(op, "name", &name) || !schema) {
      continue;
    }
    (void)vkr_bakery_json_get_string(op, "description", &description);
    char tool[160];
    mcp_tool_name(name, tool, sizeof(tool));
    VkrBakeryJson *entry = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, entry, "name",
                        vkr_bakery_json_cstr(arena, tool));
    vkr_bakery_json_set(arena, entry, "title",
                        vkr_bakery_json_string(arena, name));
    vkr_bakery_json_set(arena, entry, "description",
                        vkr_bakery_json_string(arena, description));
    vkr_bakery_json_set(arena, entry, "inputSchema",
                        vkr_bakery_json_clone(arena, schema));
    vkr_bakery_json_append(tools, entry);
  }
  VkrBakeryJson *result = mcp_result(arena);
  vkr_bakery_json_set(arena, result, "tools", tools);
  mcp_cache(arena, result, 60000);
  mcp_send_result(arena, id, result);
}

static const char s_base64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Base64 of a whole file, or NULL. */
static VkrBakeryJson *mcp_file_base64(Arena *arena, const char *path) {
  FILE *file = file_fopen(path, "rb");
  if (!file) {
    return NULL;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  const long size = ftell(file);
  if (size <= 0 || size > (long)MB(64) || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  uint8_t *bytes = arena_alloc(arena, (uint64_t)size, ARENA_MEMORY_TAG_BUFFER);
  const uint64_t encoded = ((uint64_t)size + 2u) / 3u * 4u;
  char *text = arena_alloc(arena, encoded + 1u, ARENA_MEMORY_TAG_STRING);
  if (!bytes || !text || fread(bytes, 1, (size_t)size, file) != (size_t)size) {
    fclose(file);
    return NULL;
  }
  fclose(file);
  uint64_t out = 0u;
  for (uint64_t i = 0; i < (uint64_t)size; i += 3u) {
    const uint32_t remaining = (uint32_t)Min((uint64_t)size - i, (uint64_t)3u);
    const uint32_t word = ((uint32_t)bytes[i] << 16) |
                          (remaining > 1u ? (uint32_t)bytes[i + 1u] << 8 : 0u) |
                          (remaining > 2u ? (uint32_t)bytes[i + 2u] : 0u);
    text[out++] = s_base64[(word >> 18) & 63u];
    text[out++] = s_base64[(word >> 12) & 63u];
    text[out++] = remaining > 1u ? s_base64[(word >> 6) & 63u] : '=';
    text[out++] = remaining > 2u ? s_base64[word & 63u] : '=';
  }
  text[out] = '\0';
  return vkr_bakery_json_string(
      arena, (String8){.str = (uint8_t *)text, .length = out});
}

static void mcp_tools_call(McpState *state, Arena *arena,
                           const VkrBakeryJson *id,
                           const VkrBakeryJson *params) {
  String8 tool = {0};
  if (!params || !vkr_bakery_json_get_string(params, "name", &tool)) {
    mcp_error(arena, id, MCP_INVALID_PARAMS, "tools/call needs a tool name",
              NULL);
    return;
  }
  /* The tool name maps back to the operation the editor listed. */
  char error[512];
  const VkrBakeryJson *ops = mcp_ops(state, arena, error, sizeof(error));
  if (!ops) {
    VkrBakeryJson *result = mcp_result(arena);
    VkrBakeryJson *content = vkr_bakery_json_array(arena);
    VkrBakeryJson *text = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, text, "type",
                        vkr_bakery_json_cstr(arena, "text"));
    vkr_bakery_json_set(arena, text, "text",
                        vkr_bakery_json_cstr(arena, error));
    vkr_bakery_json_append(content, text);
    vkr_bakery_json_set(arena, result, "content", content);
    vkr_bakery_json_set(arena, result, "isError",
                        vkr_bakery_json_bool(arena, 1));
    mcp_send_result(arena, id, result);
    return;
  }
  const char *op_name = NULL;
  for (const VkrBakeryJson *op = ops->first; op; op = op->next) {
    String8 name = {0};
    char candidate[160];
    if (vkr_bakery_json_get_string(op, "name", &name)) {
      mcp_tool_name(name, candidate, sizeof(candidate));
      if (strlen(candidate) == tool.length &&
          MemCompare(candidate, tool.str, tool.length) == 0) {
        op_name =
            vkr_bakery_json_cstr_value(arena, vkr_bakery_json_get(op, "name"));
        break;
      }
    }
  }
  if (!op_name) {
    char message[256];
    snprintf(message, sizeof(message), "Unknown tool: %.*s", (int)tool.length,
             tool.str);
    mcp_error(arena, id, MCP_INVALID_PARAMS, message, NULL);
    return;
  }
  const VkrBakeryJson *response = mcp_editor_call(
      &state->editor, arena, op_name, vkr_bakery_json_get(params, "arguments"),
      error, sizeof(error));
  VkrBakeryJson *result = mcp_result(arena);
  VkrBakeryJson *content = vkr_bakery_json_array(arena);
  VkrBakeryJson *text = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, text, "type", vkr_bakery_json_cstr(arena, "text"));
  bool8_t ok = false_v;
  const VkrBakeryJson *value = NULL;
  if (response && vkr_bakery_json_get_bool(response, "ok", &ok) && ok) {
    value = vkr_bakery_json_get(response, "result");
  }
  if (value) {
    String8 json = {0};
    (void)vkr_bakery_json_write(arena, value, VKR_BAKERY_JSON_COMPACT, &json);
    vkr_bakery_json_set(arena, text, "text",
                        vkr_bakery_json_string(arena, json));
    vkr_bakery_json_append(content, text);
    vkr_bakery_json_set(arena, result, "structuredContent",
                        vkr_bakery_json_clone(arena, value));
    String8 path = {0};
    if (strcmp(op_name, "view.capture") == 0 &&
        vkr_bakery_json_get_string(value, "path", &path)) {
      const char *file =
          vkr_bakery_json_cstr_value(arena, vkr_bakery_json_get(value, "path"));
      VkrBakeryJson *data = file ? mcp_file_base64(arena, file) : NULL;
      if (data) {
        VkrBakeryJson *image = vkr_bakery_json_object(arena);
        vkr_bakery_json_set(arena, image, "type",
                            vkr_bakery_json_cstr(arena, "image"));
        vkr_bakery_json_set(arena, image, "data", data);
        vkr_bakery_json_set(arena, image, "mimeType",
                            vkr_bakery_json_cstr(arena, "image/png"));
        vkr_bakery_json_append(content, image);
      }
    }
    vkr_bakery_json_set(arena, result, "isError",
                        vkr_bakery_json_bool(arena, 0));
  } else {
    char message[1024];
    const VkrBakeryJson *editor_error =
        response ? vkr_bakery_json_get(response, "error") : NULL;
    String8 code = {0};
    String8 detail = {0};
    if (editor_error) {
      (void)vkr_bakery_json_get_string(editor_error, "code", &code);
      (void)vkr_bakery_json_get_string(editor_error, "message", &detail);
      snprintf(message, sizeof(message), "%.*s: %.*s", (int)code.length,
               code.str, (int)detail.length, detail.str);
    } else {
      snprintf(message, sizeof(message), "%s", error);
    }
    vkr_bakery_json_set(arena, text, "text",
                        vkr_bakery_json_cstr(arena, message));
    vkr_bakery_json_append(content, text);
    vkr_bakery_json_set(arena, result, "isError",
                        vkr_bakery_json_bool(arena, 1));
  }
  vkr_bakery_json_set(arena, result, "content", content);
  mcp_send_result(arena, id, result);
}

static void mcp_resources_list(Arena *arena, const VkrBakeryJson *id) {
  VkrBakeryJson *resource = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, resource, "uri",
                      vkr_bakery_json_cstr(arena, MCP_FEED_URI));
  vkr_bakery_json_set(arena, resource, "name",
                      vkr_bakery_json_cstr(arena, "changes"));
  vkr_bakery_json_set(arena, resource, "title",
                      vkr_bakery_json_cstr(arena, "Change feed"));
  vkr_bakery_json_set(
      arena, resource, "description",
      vkr_bakery_json_cstr(
          arena, "The editor's change feed as changes.feed returns it: the "
                 "newest events (batches applied, changes accepted or "
                 "rejected, claims set or released) and the journal "
                 "revisions. Subscribe to learn when it moves."));
  vkr_bakery_json_set(arena, resource, "mimeType",
                      vkr_bakery_json_cstr(arena, "application/json"));
  VkrBakeryJson *resources = vkr_bakery_json_array(arena);
  vkr_bakery_json_append(resources, resource);
  VkrBakeryJson *result = mcp_result(arena);
  vkr_bakery_json_set(arena, result, "resources", resources);
  mcp_cache(arena, result, 3600000);
  mcp_send_result(arena, id, result);
}

static void mcp_resource_templates_list(Arena *arena, const VkrBakeryJson *id) {
  VkrBakeryJson *entry = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, entry, "uriTemplate",
                      vkr_bakery_json_cstr(arena, MCP_FEED_TEMPLATE));
  vkr_bakery_json_set(arena, entry, "name",
                      vkr_bakery_json_cstr(arena, "changes-after"));
  vkr_bakery_json_set(arena, entry, "title",
                      vkr_bakery_json_cstr(arena, "Change feed after"));
  vkr_bakery_json_set(
      arena, entry, "description",
      vkr_bakery_json_cstr(arena, "The change feed's events after sequence "
                                  "'after', the 'next' of your last read."));
  vkr_bakery_json_set(arena, entry, "mimeType",
                      vkr_bakery_json_cstr(arena, "application/json"));
  VkrBakeryJson *templates = vkr_bakery_json_array(arena);
  vkr_bakery_json_append(templates, entry);
  VkrBakeryJson *result = mcp_result(arena);
  vkr_bakery_json_set(arena, result, "resourceTemplates", templates);
  mcp_cache(arena, result, 3600000);
  mcp_send_result(arena, id, result);
}

/* The sequence a change-feed URI reads after: `vkr://editor/changes` or
   `vkr://editor/changes?after=<n>`. False for any other URI. */
static bool8_t mcp_feed_uri(String8 uri, int64_t *out_after) {
  static const char query[] = "?after=";
  const uint64_t base = sizeof(MCP_FEED_URI) - 1u;
  const uint64_t query_length = sizeof(query) - 1u;
  if (uri.length < base || MemCompare(uri.str, MCP_FEED_URI, base) != 0) {
    return false_v;
  }
  *out_after = 0;
  if (uri.length == base) {
    return true_v;
  }
  /* At most 18 digits, so the sequence cannot overflow. */
  if (uri.length <= base + query_length ||
      uri.length > base + query_length + 18u ||
      MemCompare(uri.str + base, query, query_length) != 0) {
    return false_v;
  }
  int64_t after = 0;
  for (uint64_t i = base + query_length; i < uri.length; ++i) {
    if (uri.str[i] < '0' || uri.str[i] > '9') {
      return false_v;
    }
    after = after * 10 + (int64_t)(uri.str[i] - '0');
  }
  *out_after = after;
  return true_v;
}

static void mcp_resources_read(McpState *state, Arena *arena,
                               const VkrBakeryJson *id,
                               const VkrBakeryJson *params) {
  String8 uri = {0};
  if (!params || !vkr_bakery_json_get_string(params, "uri", &uri)) {
    mcp_error(arena, id, MCP_INVALID_PARAMS, "resources/read needs a uri",
              NULL);
    return;
  }
  int64_t after = 0;
  if (!mcp_feed_uri(uri, &after)) {
    VkrBakeryJson *data = vkr_bakery_json_object(arena);
    vkr_bakery_json_set(arena, data, "uri", vkr_bakery_json_string(arena, uri));
    mcp_error(arena, id, MCP_INVALID_PARAMS, "Resource not found", data);
    return;
  }

  VkrBakeryJson *args = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, args, "after", vkr_bakery_json_int(arena, after));
  vkr_bakery_json_set(arena, args, "limit", vkr_bakery_json_int(arena, 128));
  char error[512];
  const VkrBakeryJson *response = mcp_editor_call(
      &state->editor, arena, "changes.feed", args, error, sizeof(error));
  bool8_t ok = false_v;
  const VkrBakeryJson *value =
      response && vkr_bakery_json_get_bool(response, "ok", &ok) && ok
          ? vkr_bakery_json_get(response, "result")
          : NULL;
  String8 text = {0};
  if (!value ||
      !vkr_bakery_json_write(arena, value, VKR_BAKERY_JSON_COMPACT, &text)) {
    if (response) {
      mcp_editor_error(response, "The editor did not read its change feed",
                       error, sizeof(error));
    }
    mcp_error(arena, id, MCP_INTERNAL_ERROR, error, NULL);
    return;
  }

  VkrBakeryJson *entry = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, entry, "uri", vkr_bakery_json_string(arena, uri));
  vkr_bakery_json_set(arena, entry, "mimeType",
                      vkr_bakery_json_cstr(arena, "application/json"));
  vkr_bakery_json_set(arena, entry, "text",
                      vkr_bakery_json_string(arena, text));
  VkrBakeryJson *contents = vkr_bakery_json_array(arena);
  vkr_bakery_json_append(contents, entry);
  VkrBakeryJson *result = mcp_result(arena);
  vkr_bakery_json_set(arena, result, "contents", contents);
  /* The feed moves with every edit; subscribe instead of caching. */
  mcp_cache(arena, result, 0);
  mcp_send_result(arena, id, result);
}

/* `subscriptions/listen`: acknowledges the notifications this server
   honours (tool-list changes and the change feed) and keeps the request
   open; the listener thread then sends its notifications until the client
   cancels it. */
static void mcp_subscribe(McpState *state, Arena *arena,
                          const VkrBakeryJson *id,
                          const VkrBakeryJson *params) {
  const VkrBakeryJson *filter =
      params ? vkr_bakery_json_get(params, "notifications") : NULL;
  if (filter && filter->type != VKR_BAKERY_JSON_OBJECT) {
    mcp_error(arena, id, MCP_INVALID_PARAMS,
              "subscriptions/listen needs a notifications object", NULL);
    return;
  }
  McpSubscription subscription = {0};
  (void)vkr_bakery_json_get_bool(filter, "toolsListChanged",
                                 &subscription.tools);
  const VkrBakeryJson *uris =
      filter ? vkr_bakery_json_get(filter, "resourceSubscriptions") : NULL;
  if (uris && uris->type == VKR_BAKERY_JSON_ARRAY) {
    for (const VkrBakeryJson *uri = uris->first; uri; uri = uri->next) {
      if (vkr_bakery_json_is_string(uri, MCP_FEED_URI)) {
        subscription.changes = true_v;
      }
    }
  }
  String8 id_text = {0};
  if (!vkr_bakery_json_write(arena, id, VKR_BAKERY_JSON_COMPACT, &id_text) ||
      id_text.length >= MCP_SUBSCRIPTION_ID_MAX) {
    mcp_error(arena, id, MCP_INVALID_REQUEST,
              "A subscription id must be shorter than 128 bytes", NULL);
    return;
  }
  MemCopy(subscription.id, id_text.str, id_text.length);
  subscription.id[id_text.length] = '\0';
  const bool8_t wanted = subscription.tools || subscription.changes;
  if (wanted && !mcp_listener_start(state)) {
    mcp_error(arena, id, MCP_INTERNAL_ERROR,
              "Notifications are unavailable: the listener did not start",
              NULL);
    return;
  }

  /* The acknowledgement names only what this server sends. */
  VkrBakeryJson *honoured = vkr_bakery_json_object(arena);
  if (subscription.tools) {
    vkr_bakery_json_set(arena, honoured, "toolsListChanged",
                        vkr_bakery_json_bool(arena, true_v));
  }
  if (subscription.changes) {
    VkrBakeryJson *list = vkr_bakery_json_array(arena);
    vkr_bakery_json_append(list, vkr_bakery_json_cstr(arena, MCP_FEED_URI));
    vkr_bakery_json_set(arena, honoured, "resourceSubscriptions", list);
  }
  VkrBakeryJson *meta = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, meta, "io.modelcontextprotocol/subscriptionId",
                      vkr_bakery_json_clone(arena, id));
  VkrBakeryJson *ack_params = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, ack_params, "_meta", meta);
  vkr_bakery_json_set(arena, ack_params, "notifications", honoured);
  VkrBakeryJson *ack = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, ack, "jsonrpc",
                      vkr_bakery_json_cstr(arena, "2.0"));
  vkr_bakery_json_set(
      arena, ack, "method",
      vkr_bakery_json_cstr(arena, "notifications/subscriptions/acknowledged"));
  vkr_bakery_json_set(arena, ack, "params", ack_params);
  String8 ack_text = {0};
  if (!vkr_bakery_json_write(arena, ack, VKR_BAKERY_JSON_COMPACT, &ack_text)) {
    mcp_error(arena, id, MCP_INTERNAL_ERROR,
              "The acknowledgement could not be serialized", NULL);
    return;
  }

  /* The acknowledgement and the new subscription enter under one lock, so
     no notification for it precedes the acknowledgement. */
  McpListener *listener = &state->listener;
  const char *refusal = NULL;
  bool8_t first = false_v;
  vkr_mutex_lock(s_output_mutex);
  for (uint32_t i = 0; wanted && i < listener->subscription_count; ++i) {
    if (strcmp(listener->subscriptions[i].id, subscription.id) == 0) {
      refusal = "A subscription with this id is open";
    }
  }
  if (!refusal && wanted &&
      listener->subscription_count >= MCP_SUBSCRIPTION_MAX) {
    refusal = "This server keeps at most 16 subscriptions open";
  }
  if (!refusal) {
    mcp_write_line_locked(ack_text.str, ack_text.length);
    if (wanted) {
      first = listener->subscription_count == 0u;
      listener->subscriptions[listener->subscription_count++] = subscription;
    }
  }
  vkr_mutex_unlock(s_output_mutex);
  if (refusal) {
    mcp_error(arena, id, MCP_INVALID_REQUEST, refusal, NULL);
    return;
  }
  if (first) {
    mcp_listener_wake(listener);
  }
}

/* `notifications/cancelled` for an open subscription ends it; nothing more
   is sent for it. Other ids are requests that already finished. */
static void mcp_cancel(McpState *state, Arena *arena,
                       const VkrBakeryJson *params) {
  const VkrBakeryJson *request =
      params ? vkr_bakery_json_get(params, "requestId") : NULL;
  String8 text = {0};
  if (!request ||
      !vkr_bakery_json_write(arena, request, VKR_BAKERY_JSON_COMPACT, &text)) {
    return;
  }
  McpListener *listener = &state->listener;
  bool8_t last = false_v;
  vkr_mutex_lock(s_output_mutex);
  for (uint32_t i = 0; i < listener->subscription_count; ++i) {
    const char *open = listener->subscriptions[i].id;
    if (strlen(open) == text.length &&
        MemCompare(open, text.str, text.length) == 0) {
      listener->subscriptions[i] =
          listener->subscriptions[--listener->subscription_count];
      last = listener->subscription_count == 0u;
      break;
    }
  }
  vkr_mutex_unlock(s_output_mutex);
  if (last) {
    mcp_listener_wake(listener);
  }
}

/* Handles one JSON-RPC message from the client. */
static void mcp_handle(McpState *state, Arena *arena, String8 line) {
  VkrBakeryJsonError parse_error = {0};
  const VkrBakeryJson *message =
      vkr_bakery_json_parse(arena, line.str, line.length, 64u, &parse_error);
  if (!message || message->type != VKR_BAKERY_JSON_OBJECT) {
    mcp_error(arena, NULL, MCP_PARSE_ERROR,
              message ? "A message must be a JSON object" : parse_error.message,
              NULL);
    return;
  }
  const VkrBakeryJson *id = vkr_bakery_json_get(message, "id");
  String8 method = {0};
  if (!vkr_bakery_json_get_string(message, "method", &method)) {
    /* A response to a server request; this server sends none. */
    if (id) {
      mcp_error(arena, id, MCP_INVALID_REQUEST, "A request needs a method",
                NULL);
    }
    return;
  }
  const VkrBakeryJson *params = vkr_bakery_json_get(message, "params");
  /* Notifications have no id and need no answer; a cancellation may end a
     subscription. */
  if (!id) {
    if (vkr_string8_equals_cstr(&method, "notifications/cancelled")) {
      mcp_cancel(state, arena, params);
    }
    return;
  }
  if (vkr_string8_equals_cstr(&method, "initialize")) {
    mcp_unsupported(arena, id,
                    params ? vkr_bakery_json_get(params, "protocolVersion")
                           : NULL);
    return;
  }
  const VkrBakeryJson *meta =
      params ? vkr_bakery_json_get(params, "_meta") : NULL;
  const VkrBakeryJson *version =
      meta
          ? vkr_bakery_json_get(meta, "io.modelcontextprotocol/protocolVersion")
          : NULL;
  if (!version || !vkr_bakery_json_is_string(version, MCP_VERSION)) {
    mcp_unsupported(arena, id, version);
    return;
  }
  if (vkr_string8_equals_cstr(&method, "server/discover")) {
    mcp_discover(arena, id);
  } else if (vkr_string8_equals_cstr(&method, "tools/list")) {
    mcp_tools_list(state, arena, id);
  } else if (vkr_string8_equals_cstr(&method, "tools/call")) {
    mcp_tools_call(state, arena, id, params);
  } else if (vkr_string8_equals_cstr(&method, "resources/list")) {
    mcp_resources_list(arena, id);
  } else if (vkr_string8_equals_cstr(&method, "resources/templates/list")) {
    mcp_resource_templates_list(arena, id);
  } else if (vkr_string8_equals_cstr(&method, "resources/read")) {
    mcp_resources_read(state, arena, id, params);
  } else if (vkr_string8_equals_cstr(&method, "subscriptions/listen")) {
    mcp_subscribe(state, arena, id, params);
  } else {
    char text[160];
    snprintf(text, sizeof(text), "Method not found: %.*s", (int)method.length,
             method.str);
    mcp_error(arena, id, MCP_METHOD_NOT_FOUND, text, NULL);
  }
}

/* The editor's default socket (editor_agent.c): `editor-<uid>.sock` in the
   per-user directory. */
static void mcp_default_socket(char *out, uint64_t capacity) {
  char name[64];
  snprintf(name, sizeof(name), "editor-%u.sock", vkr_local_socket_user_id());
  if (!vkr_local_socket_user_path(name, out, capacity)) {
    snprintf(out, capacity, "%s", name);
  }
}

int main(int argc, char **argv) {
  McpState state = {0};
#if defined(_WIN32)
  /* JSON-RPC lines end in a bare line feed; text mode would add returns. */
  (void)_setmode(_fileno(stdin), _O_BINARY);
  (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
  const char *socket = getenv("VKR_EDITOR_AGENT_SOCKET");
  const char *agent = getenv("VKR_AGENT_NAME");
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
      socket = argv[++i];
    } else if (strcmp(argv[i], "--agent") == 0 && i + 1 < argc) {
      agent = argv[++i];
    } else if (strcmp(argv[i], "--help") == 0) {
      printf("Usage: vkr_mcp [--socket <path>] [--agent <name>]\n"
             "MCP %s server over stdio for the VKR editor's agent channel.\n"
             "--agent (or VKR_AGENT_NAME) names the author of every request "
             "unless a call names its own.\n",
             MCP_VERSION);
      return 0;
    }
  }
  if (agent && agent[0]) {
    snprintf(state.agent, sizeof(state.agent), "%s", agent);
  }
  if (socket && socket[0]) {
    snprintf(state.socket_path, sizeof(state.socket_path), "%s", socket);
  } else {
    mcp_default_socket(state.socket_path, sizeof(state.socket_path));
  }
  state.editor = (McpEditor){
      .socket_path = state.socket_path,
      .agent = state.agent,
      .socket = VKR_LOCAL_SOCKET_INVALID,
  };
  state.listener.wake[0] = VKR_LOCAL_SOCKET_INVALID;
  state.listener.wake[1] = VKR_LOCAL_SOCKET_INVALID;

  Arena *arena = arena_create(MB(512), MB(1));
  state.sync_arena = arena_create(KB(64), KB(64));
  state.sync_allocator = (VkrAllocator){.ctx = state.sync_arena};
  if (!arena || !state.sync_arena ||
      !vkr_allocator_arena(&state.sync_allocator) ||
      !vkr_mutex_create(&state.sync_allocator, &s_output_mutex)) {
    mcp_log("out of memory");
    return 1;
  }
  mcp_log("serving MCP %s; editor socket %s", MCP_VERSION, state.socket_path);
  uint64_t capacity = 65536u;
  char *line = malloc(capacity);
  uint64_t length = 0u;
  int c = 0;
  while (line && (c = fgetc(stdin)) != EOF) {
    if (c != '\n') {
      if (length + 1u >= capacity) {
        if (capacity >= MCP_LINE_MAX) {
          mcp_log("dropped a message longer than %llu bytes",
                  (unsigned long long)MCP_LINE_MAX);
          length = 0u;
          while ((c = fgetc(stdin)) != EOF && c != '\n') {
          }
          continue;
        }
        char *grown = realloc(line, capacity * 2u);
        if (!grown) {
          break;
        }
        line = grown;
        capacity *= 2u;
      }
      line[length++] = (char)c;
      continue;
    }
    if (length && line[length - 1u] == '\r') {
      length--;
    }
    if (length) {
      mcp_handle(&state, arena,
                 (String8){.str = (uint8_t *)line, .length = length});
      arena_clear(arena, ARENA_MEMORY_TAG_STRUCT);
    }
    length = 0u;
  }
  mcp_listener_stop(&state);
  free(line);
  free(state.editor.pending);
  free(state.ops_text);
  mcp_editor_close(&state.editor);
  (void)vkr_mutex_destroy(&state.sync_allocator, &s_output_mutex);
  arena_destroy(state.sync_arena);
  arena_destroy(arena);
  return 0;
}
