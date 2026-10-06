/* vkr_mcp: an MCP server over stdio for the VKR editor's agent channel
 * (ADR-084).
 *
 * It speaks MCP revision 2026-07-28 only: one JSON-RPC message per line,
 * no initialize handshake, and the protocol version in each request's
 * `_meta`. Every editor operation from `ops.list` becomes the tool
 * `vkr_<operation with dots as underscores>`; a call forwards to the editor
 * socket and returns the operation's JSON as structured content. A capture
 * also returns its PNG as image content. `--agent <name>` names every
 * request's author unless a call names its own. Logs go to stderr. */

#include "defines.h"
#include "filesystem/filesystem.h"
#include "memory/arena.h"
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
    "'next' you saw to learn what others changed. Prefer blockout shapes, "
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

typedef struct McpState {
  char socket_path[512];
  /* The author every request names unless its arguments name one. */
  char agent[32];
  VkrLocalSocket socket;
  uint64_t next_id;
  /* Bytes received after the last complete editor line. */
  char *pending;
  uint64_t pending_length;
  uint64_t pending_capacity;
  /* The editor's `ops.list` result as JSON text, kept for the connection,
     so a tool call costs the editor one request instead of two. */
  char *ops_text;
  uint64_t ops_length;
} McpState;

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

static void mcp_write(Arena *arena, const VkrBakeryJson *message) {
  String8 text = {0};
  if (!vkr_bakery_json_write(arena, message, VKR_BAKERY_JSON_COMPACT, &text)) {
    mcp_log("a response could not be serialized");
    return;
  }
  fwrite(text.str, 1, text.length, stdout);
  fputc('\n', stdout);
  fflush(stdout);
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

static void mcp_send_result(Arena *arena, const VkrBakeryJson *id,
                            VkrBakeryJson *result) {
  VkrBakeryJson *message = mcp_response(arena, id);
  vkr_bakery_json_set(arena, message, "result", result);
  mcp_write(arena, message);
}

// =============================================================================
// Editor socket
// =============================================================================

static void mcp_disconnect(McpState *state) {
  vkr_local_socket_close(state->socket);
  state->socket = VKR_LOCAL_SOCKET_INVALID;
  state->pending_length = 0u;
}

static bool8_t mcp_connect(McpState *state) {
  if (state->socket != VKR_LOCAL_SOCKET_INVALID) {
    return true_v;
  }
  if (!vkr_local_socket_connect(state->socket_path, &state->socket)) {
    return false_v;
  }
  state->pending_length = 0u;
  /* A new connection can reach a restarted editor with other operations. */
  free(state->ops_text);
  state->ops_text = NULL;
  state->ops_length = 0u;
  return true_v;
}

/* Reads the next complete line from the editor into the arena. */
static bool8_t mcp_read_line(McpState *state, Arena *arena, String8 *out) {
  for (;;) {
    for (uint64_t i = 0; i < state->pending_length; ++i) {
      if (state->pending[i] != '\n') {
        continue;
      }
      char *line = arena_alloc(arena, i + 1u, ARENA_MEMORY_TAG_STRING);
      if (!line) {
        return false_v;
      }
      MemCopy(line, state->pending, i);
      line[i] = '\0';
      MemCopy(state->pending, state->pending + i + 1u,
              state->pending_length - i - 1u);
      state->pending_length -= i + 1u;
      *out = (String8){.str = (uint8_t *)line, .length = i};
      return true_v;
    }
    if (state->pending_length + 65536u > state->pending_capacity) {
      const uint64_t capacity =
          Max(state->pending_capacity * 2u, (uint64_t)131072u);
      if (capacity > MCP_LINE_MAX) {
        return false_v;
      }
      char *grown = realloc(state->pending, capacity);
      if (!grown) {
        return false_v;
      }
      state->pending = grown;
      state->pending_capacity = capacity;
    }
    uint64_t got = 0u;
    if (vkr_local_socket_recv(state->socket,
                              state->pending + state->pending_length,
                              state->pending_capacity - state->pending_length,
                              &got) != VKR_LOCAL_SOCKET_OK) {
      return false_v;
    }
    state->pending_length += got;
  }
}

/* Runs one editor operation and returns its response object, or NULL with
   `error` set when the editor cannot be reached. */
static const VkrBakeryJson *mcp_editor_call(McpState *state, Arena *arena,
                                            const char *op,
                                            const VkrBakeryJson *args,
                                            char *error, uint64_t capacity) {
  VkrBakeryJson *request = vkr_bakery_json_object(arena);
  vkr_bakery_json_set(arena, request, "v", vkr_bakery_json_int(arena, 1));
  vkr_bakery_json_set(arena, request, "id",
                      vkr_bakery_json_int(arena, (int64_t)++state->next_id));
  vkr_bakery_json_set(arena, request, "op", vkr_bakery_json_cstr(arena, op));
  if (state->agent[0]) {
    vkr_bakery_json_set(arena, request, "agent",
                        vkr_bakery_json_cstr(arena, state->agent));
  }
  vkr_bakery_json_set(arena, request, "args",
                      args && args->type == VKR_BAKERY_JSON_OBJECT
                          ? vkr_bakery_json_clone(arena, args)
                          : vkr_bakery_json_object(arena));
  String8 text = {0};
  if (!vkr_bakery_json_write(arena, request, VKR_BAKERY_JSON_COMPACT, &text)) {
    snprintf(error, capacity, "The arguments could not be serialized");
    return NULL;
  }
  /* One reconnect covers an editor that restarted since the last call. */
  for (uint32_t attempt = 0; attempt < 2u; ++attempt) {
    if (!mcp_connect(state)) {
      snprintf(error, capacity,
               "The VKR editor is not running or has its agent channel off "
               "(no listener at %s). Start the editor and retry.",
               state->socket_path);
      return NULL;
    }
    String8 line = {0};
    if (vkr_local_socket_send_all(state->socket, text.str, text.length) &&
        vkr_local_socket_send_all(state->socket, "\n", 1u) &&
        mcp_read_line(state, arena, &line)) {
      const VkrBakeryJson *response =
          vkr_bakery_json_parse(arena, line.str, line.length, 64u, NULL);
      if (!response) {
        snprintf(error, capacity, "The editor sent malformed JSON");
      }
      return response;
    }
    mcp_disconnect(state);
  }
  snprintf(error, capacity, "The editor closed the connection");
  return NULL;
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
  vkr_bakery_json_set(arena, capabilities, "tools",
                      vkr_bakery_json_object(arena));
  vkr_bakery_json_set(arena, result, "capabilities", capabilities);
  vkr_bakery_json_set(arena, result, "instructions",
                      vkr_bakery_json_cstr(arena, s_instructions));
  vkr_bakery_json_set(arena, result, "ttlMs",
                      vkr_bakery_json_int(arena, 3600000));
  vkr_bakery_json_set(arena, result, "cacheScope",
                      vkr_bakery_json_cstr(arena, "private"));
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
  if (state->ops_text && state->socket != VKR_LOCAL_SOCKET_INVALID) {
    const VkrBakeryJson *kept = vkr_bakery_json_parse(
        arena, (const uint8_t *)state->ops_text, state->ops_length, 64u, NULL);
    const VkrBakeryJson *ops = kept ? vkr_bakery_json_get(kept, "ops") : NULL;
    if (ops && ops->type == VKR_BAKERY_JSON_ARRAY) {
      return ops;
    }
  }
  const VkrBakeryJson *response =
      mcp_editor_call(state, arena, "ops.list", NULL, error, capacity);
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
    const VkrBakeryJson *editor_error = vkr_bakery_json_get(response, "error");
    String8 message = {0};
    if (editor_error &&
        vkr_bakery_json_get_string(editor_error, "message", &message)) {
      snprintf(error, capacity, "%.*s", (int)message.length,
               (const char *)message.str);
    } else {
      snprintf(error, capacity, "The editor did not list its operations");
    }
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
  vkr_bakery_json_set(arena, result, "ttlMs",
                      vkr_bakery_json_int(arena, 60000));
  vkr_bakery_json_set(arena, result, "cacheScope",
                      vkr_bakery_json_cstr(arena, "private"));
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
      state, arena, op_name, vkr_bakery_json_get(params, "arguments"), error,
      sizeof(error));
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
  /* Notifications have no id and need no answer. */
  if (!id) {
    return;
  }
  const VkrBakeryJson *params = vkr_bakery_json_get(message, "params");
  if (method.length == 10u && MemCompare(method.str, "initialize", 10u) == 0) {
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
  if (method.length == 15u &&
      MemCompare(method.str, "server/discover", 15u) == 0) {
    mcp_discover(arena, id);
  } else if (method.length == 10u &&
             MemCompare(method.str, "tools/list", 10u) == 0) {
    mcp_tools_list(state, arena, id);
  } else if (method.length == 10u &&
             MemCompare(method.str, "tools/call", 10u) == 0) {
    mcp_tools_call(state, arena, id, params);
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
  McpState state = {.socket = VKR_LOCAL_SOCKET_INVALID};
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
  Arena *arena = arena_create(MB(512), MB(1));
  if (!arena) {
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
  free(line);
  free(state.pending);
  free(state.ops_text);
  mcp_disconnect(&state);
  arena_destroy(arena);
  return 0;
}
