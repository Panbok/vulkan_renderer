#include "fps_weapon.h"

bool8_t fps_weapon_initialize(FpsWeaponState *weapon,
                              const FpsWeaponConfig *config,
                              uint32_t magazine_rounds, uint64_t instance_id) {
  if (!weapon || !config || !instance_id || !config->magazine_capacity ||
      !config->fire_interval_ticks || !config->reload_ticks ||
      magazine_rounds > config->magazine_capacity) {
    return false_v;
  }

  *weapon = (FpsWeaponState){
      .config = *config,
      .instance_id = instance_id,
      .magazine_rounds = magazine_rounds,
  };
  return true_v;
}

FpsWeaponResult fps_weapon_try_fire(FpsWeaponState *weapon, uint64_t tick,
                                    FpsWeaponReserveShot reserve_shot,
                                    void *context, FpsWeaponShot *shot) {
  if (!weapon || !weapon->instance_id || !reserve_shot || !shot) {
    return FPS_WEAPON_INVALID;
  }
  for (uint32_t i = 0; i < ArrayCount(weapon->blocks); ++i) {
    if (weapon->blocks[i]) {
      return FPS_WEAPON_BLOCKED;
    }
  }
  if (weapon->reloading) {
    return FPS_WEAPON_RELOADING;
  }
  if (tick < weapon->next_fire_tick) {
    return FPS_WEAPON_COOLDOWN;
  }
  if (!weapon->magazine_rounds) {
    return FPS_WEAPON_EMPTY;
  }
  if (weapon->shot_sequence == UINT64_MAX ||
      tick > UINT64_MAX - weapon->config.fire_interval_ticks) {
    return FPS_WEAPON_LIMIT;
  }

  const FpsWeaponShot candidate = {
      .instance_id = weapon->instance_id,
      .sequence = weapon->shot_sequence + 1u,
      .tick = tick,
  };
  if (!reserve_shot(&candidate, context)) {
    return FPS_WEAPON_CAPACITY;
  }

  weapon->magazine_rounds--;
  weapon->shot_sequence = candidate.sequence;
  weapon->next_fire_tick = tick + weapon->config.fire_interval_ticks;
  *shot = candidate;
  return FPS_WEAPON_OK;
}

FpsWeaponResult fps_weapon_reload_start(FpsWeaponState *weapon, uint64_t tick,
                                        uint32_t available_reserve,
                                        FpsWeaponReloadToken *token) {
  if (!weapon || !weapon->instance_id || !token) {
    return FPS_WEAPON_INVALID;
  }
  if (weapon->reloading) {
    return FPS_WEAPON_RELOADING;
  }
  if (weapon->magazine_rounds == weapon->config.magazine_capacity) {
    return FPS_WEAPON_FULL;
  }
  if (!available_reserve) {
    return FPS_WEAPON_NO_RESERVE;
  }
  if (weapon->reload_sequence == UINT64_MAX ||
      tick > UINT64_MAX - weapon->config.reload_ticks) {
    return FPS_WEAPON_LIMIT;
  }

  weapon->reload_sequence++;
  weapon->reload_complete_tick = tick + weapon->config.reload_ticks;
  weapon->reloading = true_v;
  *token = (FpsWeaponReloadToken){
      .instance_id = weapon->instance_id,
      .sequence = weapon->reload_sequence,
  };
  return FPS_WEAPON_OK;
}

static bool8_t weapon_reload_matches(const FpsWeaponState *weapon,
                                     FpsWeaponReloadToken token) {
  return weapon && weapon->instance_id && weapon->reloading &&
         token.instance_id == weapon->instance_id &&
         token.sequence == weapon->reload_sequence;
}

FpsWeaponResult fps_weapon_reload_complete(FpsWeaponState *weapon,
                                           FpsWeaponReloadToken token,
                                           uint64_t tick,
                                           uint32_t *reserve_rounds,
                                           uint32_t *transferred) {
  if (!reserve_rounds || !transferred || reserve_rounds == transferred) {
    return FPS_WEAPON_INVALID;
  }
  if (!weapon_reload_matches(weapon, token)) {
    return FPS_WEAPON_STALE_ACTION;
  }
  if (tick < weapon->reload_complete_tick) {
    return FPS_WEAPON_NOT_READY;
  }

  const uint32_t missing =
      weapon->config.magazine_capacity - weapon->magazine_rounds;
  const uint32_t amount = Min(missing, *reserve_rounds);
  weapon->magazine_rounds += amount;
  *reserve_rounds -= amount;
  weapon->reloading = false_v;
  weapon->reload_complete_tick = 0;
  *transferred = amount;
  return FPS_WEAPON_OK;
}

bool8_t fps_weapon_reload_cancel(FpsWeaponState *weapon,
                                 FpsWeaponReloadToken token) {
  if (!weapon_reload_matches(weapon, token)) {
    return false_v;
  }

  weapon->reloading = false_v;
  weapon->reload_complete_tick = 0;
  return true_v;
}

FpsWeaponResult fps_weapon_block_acquire(FpsWeaponState *weapon,
                                         FpsWeaponBlockToken *token) {
  if (!weapon || !weapon->instance_id || !token) {
    return FPS_WEAPON_INVALID;
  }
  if (weapon->block_sequence == UINT64_MAX) {
    return FPS_WEAPON_LIMIT;
  }
  for (uint32_t i = 0; i < ArrayCount(weapon->blocks); ++i) {
    if (!weapon->blocks[i]) {
      weapon->block_sequence++;
      weapon->blocks[i] = weapon->block_sequence;
      *token = (FpsWeaponBlockToken){
          .instance_id = weapon->instance_id,
          .sequence = weapon->block_sequence,
      };
      return FPS_WEAPON_OK;
    }
  }
  return FPS_WEAPON_CAPACITY;
}

bool8_t fps_weapon_block_release(FpsWeaponState *weapon,
                                 FpsWeaponBlockToken token) {
  if (!weapon || !weapon->instance_id || !token.sequence ||
      token.instance_id != weapon->instance_id) {
    return false_v;
  }
  for (uint32_t i = 0; i < ArrayCount(weapon->blocks); ++i) {
    if (weapon->blocks[i] == token.sequence) {
      weapon->blocks[i] = 0;
      return true_v;
    }
  }
  return false_v;
}
