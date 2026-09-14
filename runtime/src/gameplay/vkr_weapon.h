#pragma once

#include "defines.h"

#define VKR_WEAPON_MAX_BLOCKS 8u

typedef struct VkrWeaponConfig {
  uint32_t magazine_capacity;
  uint64_t fire_interval_ticks;
  uint64_t reload_ticks;
} VkrWeaponConfig;

typedef struct VkrWeaponShot {
  uint64_t instance_id;
  uint64_t sequence;
  uint64_t tick;
} VkrWeaponShot;

typedef struct VkrWeaponReloadToken {
  uint64_t instance_id;
  uint64_t sequence;
} VkrWeaponReloadToken;

typedef struct VkrWeaponBlockToken {
  uint64_t instance_id;
  uint64_t sequence;
} VkrWeaponBlockToken;

typedef enum VkrWeaponResult {
  VKR_WEAPON_OK,
  VKR_WEAPON_INVALID,
  VKR_WEAPON_BLOCKED,
  VKR_WEAPON_RELOADING,
  VKR_WEAPON_COOLDOWN,
  VKR_WEAPON_EMPTY,
  VKR_WEAPON_CAPACITY,
  VKR_WEAPON_FULL,
  VKR_WEAPON_NO_RESERVE,
  VKR_WEAPON_NOT_READY,
  VKR_WEAPON_STALE_ACTION,
  VKR_WEAPON_LIMIT,
} VkrWeaponResult;

/* Caller-owned value storage, with no borrowed pointers or allocations. Treat
 * fields as read-only after initialization. One simulation owner serializes all
 * operations; ticks come from that owner's common simulation clock. Output and
 * sink storage must be disjoint from weapon storage. */
typedef struct VkrWeaponState {
  VkrWeaponConfig config;
  uint64_t instance_id;
  uint64_t shot_sequence;
  uint64_t next_fire_tick;
  uint64_t reload_sequence;
  uint64_t reload_complete_tick;
  uint64_t block_sequence;
  uint64_t blocks[VKR_WEAPON_MAX_BLOCKS];
  uint32_t magazine_rounds;
  bool8_t reloading;
} VkrWeaponState;

/* Copies validated configuration. Capacity and durations must be nonzero.
 * instance_id must be nonzero and unique while any token/shot from an earlier
 * initialization can remain in flight, including after slot reuse/scene reset.
 * Failure preserves the destination. Destruction only ends the caller's value
 * lifetime; the caller cancels external timers/routes before releasing it. */
bool8_t vkr_weapon_initialize(VkrWeaponState *weapon,
                              const VkrWeaponConfig *config,
                              uint32_t magazine_rounds, uint64_t instance_id);

/* The sink reserves every required shot resource/fact atomically. False means
 * no reservation escaped; true means later publication cannot fail. The shot
 * pointer is borrowed only for this call. The sink must not reenter or mutate
 * this weapon, invoke gameplay consumers, or retain the borrowed pointer. */
typedef bool8_t (*VkrWeaponReserveShot)(const VkrWeaponShot *shot,
                                        void *context);

/* Eligibility and integer overflow are checked before calling the sink. Only
 * successful reservation spends ammo and advances the sequence/deadline. Output
 * is written only on success. Input/aim permission and whole-pellet admission
 * belong to the caller. Held-fire input must call once per eligible tick;
 * this primitive owns neither trigger intent nor an independent update clock.
 */
VkrWeaponResult vkr_weapon_try_fire(VkrWeaponState *weapon, uint64_t tick,
                                    VkrWeaponReserveShot reserve_shot,
                                    void *context, VkrWeaponShot *shot);

/* Start only checks current reserve availability; it does not spend/reserve it.
 * Completion revalidates the inventory value and transfers at most the missing
 * magazine rounds. The inventory owner lends exclusive access for completion;
 * reserve_rounds must not alias weapon storage. Zero reserve at completion ends
 * the reload successfully with zero transfer. Outputs change only on success.
 */
VkrWeaponResult vkr_weapon_reload_start(VkrWeaponState *weapon, uint64_t tick,
                                        uint32_t available_reserve,
                                        VkrWeaponReloadToken *token);
VkrWeaponResult vkr_weapon_reload_complete(VkrWeaponState *weapon,
                                           VkrWeaponReloadToken token,
                                           uint64_t tick,
                                           uint32_t *reserve_rounds,
                                           uint32_t *transferred);
bool8_t vkr_weapon_reload_cancel(VkrWeaponState *weapon,
                                 VkrWeaponReloadToken token);

/* Each successful acquisition owns one independent firing-only lock. Releasing
 * a stale token cannot clear another lock. Sequences never wrap. Reload remains
 * independent: death/unequip callers explicitly cancel it with its token. The
 * input owner must cancel held-fire intent and require fresh activation when a
 * firing lock is acquired. Output changes only on successful acquisition. */
VkrWeaponResult vkr_weapon_block_acquire(VkrWeaponState *weapon,
                                         VkrWeaponBlockToken *token);
bool8_t vkr_weapon_block_release(VkrWeaponState *weapon,
                                 VkrWeaponBlockToken token);
