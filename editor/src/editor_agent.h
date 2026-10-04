#pragma once

#include "editor_ui.h"

/* The agent channel (ADR-084): a
 * per-user local socket that speaks newline-delimited JSON requests and runs
 * them through the operation table (editor_ops.h) on the UI thread. */
typedef struct VkrEditorAgent VkrEditorAgent;

/* Creates the agent and, unless `enabled` is false, its listener. `socket`
 * selects the path; NULL uses VKR_EDITOR_AGENT_SOCKET or the default path.
 * A listener that cannot start leaves the agent without one and records why
 * in its status. */
VkrEditorAgent *vkr_editor_agent_create(VkrAllocator *allocator,
                                        const char *socket, bool8_t enabled);
void vkr_editor_agent_destroy(VkrEditorAgent *agent);

/* Accepts clients, reads requests, advances the active one and writes
 * responses. Call once per UI build. */
void vkr_editor_agent_update(VkrEditorAgent *agent, VkrEditorUi *editor,
                             const VkrSampleUiFrame *frame);

/* A client is connected or a request waits or runs. A headless editor stays
   open while this holds. */
bool8_t vkr_editor_agent_busy(const VkrEditorAgent *agent);

/* "Listening on <path>", or why the channel is off. */
const char *vkr_editor_agent_status(const VkrEditorAgent *agent);

/* Queues a request from the editor itself, such as the Changes panel's
 * Reject; its response goes to the Console. `line` is one JSON request. */
bool8_t vkr_editor_agent_submit(VkrEditorAgent *agent, const char *line);

struct VkrEditorOps *vkr_editor_agent_ops(VkrEditorAgent *agent);

/* The per-user directory for the socket and captures: `$TMPDIR/vkr` (or
   `/tmp/vkr`), created with mode 0700; false unless it exists, belongs to
   this user and admits no one else. */
bool8_t vkr_editor_agent_directory(char *out, uint64_t capacity);
