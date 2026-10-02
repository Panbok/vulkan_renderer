/**
 * @file vkr_script_internal.h
 * @brief Script host state shared by the host lifecycle and the SDK
 * implementation (ADR-079). Not for other subsystems.
 */
#pragma once

#include "script/vkr_script_host.h"

/* What a ledger record releases. */
typedef enum ScriptLedgerKind {
  SCRIPT_LEDGER_NONE = 0,
  SCRIPT_LEDGER_ENTITY,
  SCRIPT_LEDGER_CHARACTER,
  SCRIPT_LEDGER_MODEL,
  SCRIPT_LEDGER_STATE,
  SCRIPT_LEDGER_RENDER_POSE,
} ScriptLedgerKind;

typedef struct ScriptLedgerRecord {
  uint64_t entity;
  /* SCRIPT_LEDGER_STATE: the host state type index. */
  uint32_t aux;
  uint32_t kind; /**< ScriptLedgerKind. */
} ScriptLedgerRecord;

/* Acquisitions of one scope, released in reverse order. */
typedef struct ScriptLedger {
  ScriptLedgerRecord *records;
  uint32_t count;
  uint32_t capacity;
} ScriptLedger;

/* One behavior running on one entity. */
typedef struct ScriptBinding {
  uint64_t entity;
  uint32_t behavior;
  ScriptLedger ledger;
  bool8_t started;
} ScriptBinding;

/* The context a hook receives; `base` is what the SDK sees. */
typedef struct ScriptCtx {
  VkrCtx base;
  VkrScriptHost *host;
  VkrScriptInstance *instance;
  /* Where acquisitions of the running hook are recorded. */
  ScriptLedger *ledger;
  char last_error[VKR_SCRIPT_ERROR_CAPACITY];
} ScriptCtx;

#define SCRIPT_MODULE_NONE UINT32_MAX

struct VkrScriptInstance {
  ScriptCtx ctx;
  /* SCRIPT_MODULE_NONE for a tool context. */
  uint32_t module;
  uint32_t container;
  void *data;
  uint32_t data_size;
  uint32_t data_align;
  ScriptLedger ledger;
  ScriptBinding *bindings;
  uint32_t binding_count;
  uint32_t binding_capacity;
  /* Container world revisions at the last behavior sync; an unchanged set
     skips the next one. */
  uint64_t synced_revisions[VKR_SCRIPT_CONTAINER_MAX];
  bool8_t synced;
  /* start ran; stop runs at the end. */
  bool8_t started;
  bool8_t disabled;
  /* The running hook called vkr_fail. */
  bool8_t failed;
};

/* Fills the SDK table (vkr_script_sdk.c). */
void script_sdk_table(VkrSdkTable *table);

/* The attached container holding `entity`, or NULL. */
VkrScriptContainer *script_container_of(VkrScriptHost *host, uint64_t entity);

/* The container slot `container` names for `ctx`, or UINT32_MAX. */
uint32_t script_container_slot(ScriptCtx *ctx, VkrContainer container);

/* Records an acquisition in the running hook's scope. False when out of
 * memory; the caller then releases what it acquired. */
bool8_t script_ledger_record(ScriptCtx *ctx, ScriptLedgerKind kind,
                             uint64_t entity, uint32_t aux);

/* Forgets a record anywhere in the context's instance after an explicit
 * release. */
void script_ledger_forget(ScriptCtx *ctx, ScriptLedgerKind kind,
                          uint64_t entity, uint32_t aux);

/* Applies pending transform and hierarchy edits of `scene` before a read:
 * world matrices and child lists are current afterwards. */
void script_refresh_scene(VkrScriptHost *host, VkrScene *scene);

/* Destroys an entity after its descendants. */
void script_destroy_tree(VkrScriptHost *host, VkrScene *scene,
                         VkrEntityId entity);

/* Runs a running binding's stop hook and releases its scope. */
void script_binding_end(VkrScriptHost *host, VkrScriptInstance *instance,
                        ScriptBinding *binding);

/* Releases one record's resource. */
void script_ledger_release_record(VkrScriptHost *host,
                                  const ScriptLedgerRecord *record);

/* The registered type a module or engine descriptor names, or NULL. */
const VkrTypeDesc *script_resolve_type(VkrScriptHost *host,
                                       const VkrComponentDesc *type);

/* Sets the context's last error and returns false. */
bool8_t script_ctx_error(ScriptCtx *ctx, const char *message);
