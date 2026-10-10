#pragma once

#include "vkr_depot_object.h"
#include "vkr_depot_store.h"
#include "vkr_net_session.h"
#include "vkr_wire.h"

/* The depot service (docs/proposals/network-protocol.md, "Depot"). Channel
 * 0 of the service carries the schema messages below; channel 1 carries
 * stored object representations as raw messages whose tag is
 * `(request << 8) | index`, naming the request and the ID's position in it.
 * Every request carries a client-chosen `request` number that its reply
 * echoes. */

#define VKR_DEPOT_SERVICE_VERSION 1u
#define VKR_DEPOT_BATCH 256u
#define VKR_DEPOT_REFS_MAX 64u
#define VKR_DEPOT_LOCKS_PAGE 32u
#define VKR_DEPOT_LOCK_PATH_MAX 512u
#define VKR_DEPOT_CONTROL_CHANNEL 0u
#define VKR_DEPOT_DATA_CHANNEL 1u

typedef enum VkrDepotMessageId {
  VKR_DEPOT_MSG_REFS_REQUEST = 1,
  VKR_DEPOT_MSG_REFS = 2,
  VKR_DEPOT_MSG_HAVE = 3,
  VKR_DEPOT_MSG_HAVE_REPLY = 4,
  VKR_DEPOT_MSG_GET = 5,
  VKR_DEPOT_MSG_GET_REPLY = 6,
  VKR_DEPOT_MSG_PUT = 7,
  VKR_DEPOT_MSG_PUT_REPLY = 8,
  VKR_DEPOT_MSG_UPDATE_REF = 9,
  VKR_DEPOT_MSG_UPDATE_REF_REPLY = 10,
  VKR_DEPOT_MSG_LOCK = 11,
  VKR_DEPOT_MSG_LOCK_REPLY = 12,
  VKR_DEPOT_MSG_LOCKS_REQUEST = 13,
  VKR_DEPOT_MSG_LOCKS = 14,
} VkrDepotMessageId;

typedef enum VkrDepotRefStatus {
  VKR_DEPOT_REF_OK = 0,
  /* The branch is not at `expected`; `current` names where it is. */
  VKR_DEPOT_REF_CONFLICT,
  /* An object the commit needs is missing on the server. */
  VKR_DEPOT_REF_INCOMPLETE,
  /* The commit changes a file another key holds a lock on. */
  VKR_DEPOT_REF_LOCKED,
  VKR_DEPOT_REF_DENIED,
  VKR_DEPOT_REF_STATUS_COUNT
} VkrDepotRefStatus;

typedef enum VkrDepotLockStatus {
  VKR_DEPOT_LOCK_OK = 0,
  /* Another key holds it; `owner` names the key. */
  VKR_DEPOT_LOCK_HELD,
  VKR_DEPOT_LOCK_NOT_HELD,
  VKR_DEPOT_LOCK_DENIED,
  VKR_DEPOT_LOCK_STATUS_COUNT
} VkrDepotLockStatus;

typedef enum VkrDepotRole {
  VKR_DEPOT_ROLE_NONE = 0,
  VKR_DEPOT_ROLE_READ = 1,
  VKR_DEPOT_ROLE_WRITE = 2,
  VKR_DEPOT_ROLE_ADMIN = 3,
} VkrDepotRole;

typedef struct VkrDepotRequestWire {
  uint64_t request;
} VkrDepotRequestWire;

typedef struct VkrDepotRefWire {
  char name[VKR_DEPOT_BRANCH_NAME];
  uint8_t commit[VKR_DEPOT_ID_SIZE];
} VkrDepotRefWire;

typedef struct VkrDepotRefsWire {
  uint64_t request;
  VkrDepotRefWire refs[VKR_DEPOT_REFS_MAX];
  uint32_t ref_count;
} VkrDepotRefsWire;

/* HAVE, GET and PUT. GET's `zstd` says the client decodes zstd. */
typedef struct VkrDepotIdsWire {
  uint64_t request;
  bool8_t zstd;
  uint8_t ids[VKR_DEPOT_BATCH][VKR_DEPOT_ID_SIZE];
  uint32_t id_count;
} VkrDepotIdsWire;

/* HAVE_REPLY: IDs the server lacks. GET_REPLY: IDs it lacks. PUT_REPLY:
   IDs whose bytes failed their check. One bit per requested ID. */
typedef struct VkrDepotBitsWire {
  uint64_t request;
  uint8_t bits[VKR_DEPOT_BATCH / 8u];
  uint32_t bits_size;
} VkrDepotBitsWire;

typedef struct VkrDepotUpdateRefWire {
  uint64_t request;
  char name[VKR_DEPOT_BRANCH_NAME];
  /* Zero creates the branch. */
  uint8_t expected[VKR_DEPOT_ID_SIZE];
  uint8_t commit[VKR_DEPOT_ID_SIZE];
} VkrDepotUpdateRefWire;

typedef struct VkrDepotUpdateRefReplyWire {
  uint64_t request;
  uint32_t status; /**< VkrDepotRefStatus. */
  uint8_t current[VKR_DEPOT_ID_SIZE];
} VkrDepotUpdateRefReplyWire;

typedef struct VkrDepotLockWire {
  uint64_t request;
  char path[VKR_DEPOT_LOCK_PATH_MAX];
  bool8_t release;
  /* An admin may release another key's lock. */
  bool8_t force;
} VkrDepotLockWire;

typedef struct VkrDepotLockReplyWire {
  uint64_t request;
  uint32_t status; /**< VkrDepotLockStatus. */
  uint8_t owner[32];
} VkrDepotLockReplyWire;

typedef struct VkrDepotLocksRequestWire {
  uint64_t request;
  /* Paths after this one, in byte order; empty from the start. */
  char after[VKR_DEPOT_LOCK_PATH_MAX];
} VkrDepotLocksRequestWire;

typedef struct VkrDepotLockEntryWire {
  char path[VKR_DEPOT_LOCK_PATH_MAX];
  uint8_t owner[32];
  uint64_t time_us;
} VkrDepotLockEntryWire;

typedef struct VkrDepotLocksWire {
  uint64_t request;
  VkrDepotLockEntryWire entries[VKR_DEPOT_LOCKS_PAGE];
  uint32_t entry_count;
  bool8_t more;
} VkrDepotLocksWire;

typedef union VkrDepotMessage {
  VkrDepotRequestWire request;
  VkrDepotRefsWire refs;
  VkrDepotIdsWire ids;
  VkrDepotBitsWire bits;
  VkrDepotUpdateRefWire update_ref;
  VkrDepotUpdateRefReplyWire update_ref_reply;
  VkrDepotLockWire lock;
  VkrDepotLockReplyWire lock_reply;
  VkrDepotLocksRequestWire locks_request;
  VkrDepotLocksWire locks;
} VkrDepotMessage;

const VkrWireSchema *vkr_depot_schema(void);
const VkrWireMessage *vkr_depot_message(VkrDepotMessageId id);

/* The depot service descriptor without callbacks: its versions and
   channels. */
VkrNetService vkr_depot_service_template(void);

/* Encodes and sends a depot message on the control channel. */
bool8_t vkr_depot_send(VkrNetSession *session, VkrNetConnectionId connection,
                       VkrDepotMessageId id, const void *message,
                       uint64_t now_us);

/* True for a branch name: 1 to 63 bytes of [A-Za-z0-9._-], not starting
   with '.'. */
bool8_t vkr_depot_branch_valid(const char *name);

static inline uint64_t vkr_depot_tag(uint64_t request, uint32_t index) {
  return (request << 8) | (uint64_t)(index & 0xffu);
}
