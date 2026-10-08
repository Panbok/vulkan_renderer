// Jolt requires its umbrella header before every other Jolt header.
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on

#include "vkr_physics.h"
extern "C" {
#include "core/logger.h"
}
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollisionDispatch.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
class s_BroadLayers final : public JPH::BroadPhaseLayerInterface {
public:
  JPH::uint GetNumBroadPhaseLayers() const override { return 1; }
  JPH::BroadPhaseLayer
  GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
    (void)layer;
    return JPH::BroadPhaseLayer(0);
  }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char *
  GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
    return layer.GetValue() == 0 ? "static" : "moving";
  }
#endif
};

class s_BroadFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
  bool ShouldCollide(JPH::ObjectLayer layer,
                     JPH::BroadPhaseLayer broad) const override {
    (void)layer;
    (void)broad;
    return true;
  }
};

class s_PairFilter final : public JPH::ObjectLayerPairFilter {
public:
  bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
    return (a & (b >> 16)) != 0 && (b & (a >> 16)) != 0;
  }
};

struct s_BodySlot {
  JPH::BodyID id;
  uint32_t generation = 1;
  bool8_t occupied = false_v;
  bool8_t enabled = false_v;
  VkrPhysicsMotion motion = VKR_PHYSICS_STATIC;
  bool8_t sensor = false_v;
  bool8_t destroy_reserved = false_v;
  uint32_t reserved_events = 0;
  uint32_t reserved_contacts = 0;
  JPH::Vec3 disabled_linear = JPH::Vec3::sZero();
  JPH::Vec3 disabled_angular = JPH::Vec3::sZero();
};

struct s_JointSlot {
  JPH::Ref<JPH::TwoBodyConstraint> constraint;
  VkrPhysicsBody body_a = 0;
  VkrPhysicsBody body_b = 0;
  uint32_t generation = 0;
  bool8_t enabled = false_v;
  bool8_t collide_connected = false_v;
};

struct s_CharacterSlot {
  JPH::Ref<JPH::CharacterVirtual> character;
  JPH::RefConst<JPH::Shape> standing_shape;
  JPH::RefConst<JPH::Shape> crouched_shape;
  VkrPhysicsCharacterDesc desc{};
  uint32_t generation = 0;
  bool8_t crouched = false_v;
};

// Jolt registration is process-wide; the C contract serializes all world calls.
uint32_t world_count = 0;
uint64_t next_body_generation = 1;
uint64_t next_joint_generation = 1;
uint64_t next_character_generation = 1;

JPH::Vec3 vec(const float32_t *v) { return JPH::Vec3(v[0], v[1], v[2]); }
JPH::Quat quat(const float32_t *q) {
  return JPH::Quat(q[0], q[1], q[2], q[3]).Normalized();
}
void store_vec(JPH::Vec3Arg v, float32_t *out) {
  out[0] = v.GetX();
  out[1] = v.GetY();
  out[2] = v.GetZ();
}
bool finite_vector(const float32_t *v, uint32_t count) {
  if (!v) {
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (!std::isfinite(v[i]) || std::abs(v[i]) > VKR_PHYSICS_MAX_COORDINATE) {
      return false;
    }
  }
  return true;
}
bool valid_rotation(const float32_t *q) {
  if (!finite_vector(q, 4)) {
    return false;
  }
  float32_t length = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
  return length > 1.0e-12f;
}
} // namespace

struct s_ContactPair {
  VkrPhysicsContactEvent event;
  JPH::BodyID id_a;
  JPH::BodyID id_b;
};

/* Contact key order: bodies, then colliders. Events follow it, so their
   order does not depend on when Jolt reported each contact. */
static bool contact_before(const s_ContactPair &a, const s_ContactPair &b) {
  if (a.event.body_a != b.event.body_a) {
    return a.event.body_a < b.event.body_a;
  }
  if (a.event.body_b != b.event.body_b) {
    return a.event.body_b < b.event.body_b;
  }
  if (a.event.collider_a != b.event.collider_a) {
    return a.event.collider_a < b.event.collider_a;
  }
  return a.event.collider_b < b.event.collider_b;
}

class s_ContactListener final : public JPH::ContactListener {
public:
  explicit s_ContactListener(VkrPhysicsWorld *world) : world(world) {}
  JPH::ValidateResult
  OnContactValidate(const JPH::Body &a, const JPH::Body &b, JPH::RVec3Arg base,
                    const JPH::CollideShapeResult &result) override;
  void OnContactAdded(const JPH::Body &a, const JPH::Body &b,
                      const JPH::ContactManifold &manifold,
                      JPH::ContactSettings &settings) override;
  void OnContactPersisted(const JPH::Body &a, const JPH::Body &b,
                          const JPH::ContactManifold &manifold,
                          JPH::ContactSettings &settings) override;
  VkrPhysicsWorld *world;
};

/* Jolt's jobs on the engine's workers (vkr_physics_set_jobs). A job the
 * workers cannot take stays with its barrier, whose Wait runs it on the
 * stepping thread; a job both reach runs once. Process-wide: steps are
 * serialized, so one set of barriers and jobs serves every world. */
class s_JobSystem final : public JPH::JobSystemWithBarrier {
public:
  explicit s_JobSystem(const VkrPhysicsJobs &host)
      : JPH::JobSystemWithBarrier(JPH::cMaxPhysicsBarriers), host(host) {
    jobs.Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsJobs);
  }

  /* Jobs handed to workers that have not let go of them yet. */
  std::atomic<uint32_t> outstanding{0};

  uint32_t min_active_bodies() const { return host.min_active_bodies; }

  int GetMaxConcurrency() const override {
    return static_cast<int>(host.worker_count) + 1;
  }

  JobHandle CreateJob(const char *name, JPH::ColorArg color,
                      const JobFunction &function,
                      JPH::uint32 dependencies) override {
    JPH::uint32 index = Jobs::cInvalidObjectIndex;
    while ((index = jobs.ConstructObject(name, color, this, function,
                                         dependencies)) ==
           Jobs::cInvalidObjectIndex) {
      std::this_thread::yield();
    }
    Job *job = &jobs.Get(index);
    // The handle keeps a reference; a queued job may finish at once.
    JobHandle handle(job);
    if (dependencies == 0) {
      QueueJob(job);
    }
    return handle;
  }

  void QueueJob(Job *job) override { submit(job); }

  void QueueJobs(Job **queued, JPH::uint count) override {
    for (JPH::uint i = 0; i < count; ++i) {
      submit(queued[i]);
    }
  }

  void FreeJob(Job *job) override { jobs.DestructObject(job); }

  /* Queues `task(arg)` on a worker, counted with the jobs so clearing waits
     for it; false when the workers cannot take it. The task calls
     task_done last. */
  bool submit_task(void (*task)(void *), void *arg) {
    outstanding.fetch_add(1, std::memory_order_relaxed);
    if (!host.submit(host.context, task, arg)) {
      outstanding.fetch_sub(1, std::memory_order_relaxed);
      return false;
    }
    return true;
  }

  void task_done() { outstanding.fetch_sub(1, std::memory_order_release); }

private:
  using Jobs = JPH::FixedSizeFreeList<Job>;

  static void run(void *arg) {
    Job *job = static_cast<Job *>(arg);
    s_JobSystem *self = static_cast<s_JobSystem *>(job->GetJobSystem());
    job->Execute();
    job->Release();
    self->outstanding.fetch_sub(1, std::memory_order_release);
  }

  void submit(Job *job) {
    job->AddRef();
    outstanding.fetch_add(1, std::memory_order_relaxed);
    if (!host.submit(host.context, run, job)) {
      job->Release();
      outstanding.fetch_sub(1, std::memory_order_relaxed);
    }
  }

  VkrPhysicsJobs host;
  Jobs jobs;
};

static s_JobSystem *s_jobs = nullptr;

struct s_VkrPhysicsWorld {
  s_BroadLayers layers;
  s_BroadFilter broad_filter;
  s_PairFilter pair_filter;
  JPH::PhysicsSystem system;
  s_ContactListener contact_listener{this};
  // Pinned Jolt 5.5 rigid-only scratch bound: eight contacts/body (<1536 bytes
  // each), CCD/island arrays (<1024 bytes/body), and fixed in-flight pairs.
  // Up to16 joints/body add <256 bytes/body of temporary pointer/index arrays.
  // No soft bodies, large-island splitter or scratch heap fallback.
  // Re-audit these allocation sites when updating Jolt.
  JPH::TempAllocatorImpl scratch;
  explicit s_VkrPhysicsWorld(uint32_t max_bodies)
      : scratch(2 * 1024 * 1024 + static_cast<size_t>(max_bodies) * 16384) {}
  JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
  /* Contact callbacks arrive from several workers at once. */
  std::mutex contact_mutex;
  std::vector<s_BodySlot> slots;
  std::vector<uint32_t> native_to_slot;
  uint32_t joint_count = 0;
  std::vector<s_JointSlot> joints;
  std::array<s_CharacterSlot, VKR_PHYSICS_MAX_CHARACTERS> characters;
  std::vector<VkrPhysicsSensorEvent> sensor_pairs;
  std::vector<VkrPhysicsSensorEvent> next_pairs;
  std::vector<VkrPhysicsSensorEvent> events;
  /* Solid contacts of the last step and of the running one, sorted by
     contact key once the step's callbacks have run. */
  std::vector<s_ContactPair> contacts;
  std::vector<s_ContactPair> next_contacts;
  /* The running step's contacts by key: open addressing over twice the
     contact capacity, a slot holding a next_contacts index and the stamp of
     the step that wrote it. */
  std::vector<uint32_t> contact_slots;
  std::vector<uint32_t> contact_stamps;
  uint32_t contact_stamp = 0;
  std::vector<VkrPhysicsContactEvent> contact_events;
  const char *error = "";
  bool8_t faulted = false_v;
  bool8_t dispatching = false_v;
  uint32_t reserved_events = 0;
  uint32_t reserved_destructions = 0;
  uint32_t reserved_contact_events = 0;
};

static bool8_t fail(VkrPhysicsWorld *world, const char *message) {
  if (world) {
    world->error = message;
  }
  return false_v;
}

static s_BodySlot *lookup(VkrPhysicsWorld *world, VkrPhysicsBody handle) {
  if (!world || world->faulted || !handle) {
    fail(world, "Physics world is null, faulted, or body is invalid");
    return nullptr;
  }
  uint32_t index = static_cast<uint32_t>(handle) - 1;
  uint32_t generation = static_cast<uint32_t>(handle >> 32);
  if (index >= world->slots.size() || !world->slots[index].occupied ||
      world->slots[index].generation != generation) {
    fail(world, "Stale physics body handle");
    return nullptr;
  }
  return &world->slots[index];
}

static VkrPhysicsBody handle_for(const VkrPhysicsWorld *world, uint32_t index) {
  return (static_cast<uint64_t>(world->slots[index].generation) << 32) |
         (static_cast<uint64_t>(index) + 1);
}

extern "C" VkrPhysicsWorld *vkr_physics_world_create(uint32_t max_bodies) {
  if (max_bodies == 0 || max_bodies > 65536) {
    return nullptr;
  }
  bool registered = false;
  try {
    if (world_count == 0) {
      JPH::RegisterDefaultAllocator();
      JPH::Factory::sInstance = new JPH::Factory();
      registered = true;
      JPH::RegisterTypes();
    }
    static_assert(
        UINT32_MAX /
                JPH::ContactConstraintManager::cMaxContactConstraintsLimit <
            1536,
        "Re-audit the pinned Jolt scratch bound");
    auto world = std::make_unique<VkrPhysicsWorld>(max_bodies);
    world->slots.resize(max_bodies);
    world->native_to_slot.resize(max_bodies, UINT32_MAX);
    world->joints.resize(max_bodies * VKR_PHYSICS_JOINTS_PER_BODY);
    world->sensor_pairs.reserve(max_bodies * VKR_PHYSICS_SENSOR_PAIRS_PER_BODY);
    world->next_pairs.reserve(max_bodies * VKR_PHYSICS_SENSOR_PAIRS_PER_BODY);
    world->events.reserve(max_bodies * VKR_PHYSICS_SENSOR_EVENTS_PER_BODY);
    world->contacts.reserve(max_bodies * 8);
    world->next_contacts.reserve(max_bodies * 8);
    size_t contact_slots = 1;
    while (contact_slots < static_cast<size_t>(max_bodies) * 16) {
      contact_slots <<= 1;
    }
    world->contact_slots.resize(contact_slots);
    world->contact_stamps.resize(contact_slots, 0);
    world->contact_events.reserve(max_bodies *
                                  VKR_PHYSICS_CONTACT_EVENTS_PER_BODY);
    world->system.SetContactListener(&world->contact_listener);
    world->system.Init(max_bodies, 0, max_bodies * 8, max_bodies * 8,
                       world->layers, world->broad_filter, world->pair_filter);
    auto settings = world->system.GetPhysicsSettings();
    settings.mUseLargeIslandSplitter = false;
    settings.mMinVelocityForRestitution = VKR_PHYSICS_RESTITUTION_THRESHOLD;
    world->system.SetPhysicsSettings(settings);
    world->system.SetGravity(JPH::Vec3(0, -9.81f, 0));
    ++world_count;
    return world.release();
  } catch (...) {
    if (registered) {
      JPH::UnregisterTypes();
      delete JPH::Factory::sInstance;
      JPH::Factory::sInstance = nullptr;
    }
    return nullptr;
  }
}

static void remove_attached_joints(VkrPhysicsWorld *world, VkrPhysicsBody body);
static void refresh_attached_joints(VkrPhysicsWorld *world,
                                    VkrPhysicsBody body);

extern "C" void vkr_physics_world_destroy(VkrPhysicsWorld *world) {
  if (world && world->dispatching) {
    fail(world, "Physics mutation during event dispatch");
    return;
  }

  if (!world) {
    return;
  }
  for (auto &slot : world->characters) {
    slot.character = nullptr;
    slot.standing_shape = nullptr;
    slot.crouched_shape = nullptr;
  }
  for (auto &joint : world->joints) {
    if (joint.constraint) {
      world->system.RemoveConstraint(joint.constraint);
      joint.constraint = nullptr;
    }
  }
  auto &bodies = world->system.GetBodyInterface();
  for (auto &slot : world->slots) {
    if (slot.occupied) {
      if (slot.enabled) {
        bodies.RemoveBody(slot.id);
      }
      bodies.DestroyBody(slot.id);
    }
  }
  delete world;
  if (--world_count == 0) {
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
  }
}

extern "C" const char *vkr_physics_last_error(const VkrPhysicsWorld *world) {
  return world ? world->error : "Physics world creation failed";
}

class s_CharacterFilter final : public JPH::ObjectLayerFilter,
                                public JPH::BodyFilter {
public:
  explicit s_CharacterFilter(const VkrPhysicsCharacterDesc &desc)
      : desc(desc) {}
  // Keep BodyFilter's BodyID overload visible beside the layer override.
  using JPH::BodyFilter::ShouldCollide;
  bool ShouldCollide(JPH::ObjectLayer layer) const override {
    return (desc.collision_mask & layer) != 0 &&
           (desc.collision_layer & (layer >> 16)) != 0;
  }
  bool ShouldCollideLocked(const JPH::Body &body) const override {
    return !body.IsSensor() && body.GetUserData() != desc.entity_id;
  }
  const VkrPhysicsCharacterDesc &desc;
};

static s_CharacterSlot *character_lookup(VkrPhysicsWorld *world,
                                         VkrPhysicsCharacter handle) {
  if (!world || world->faulted || !handle) {
    fail(world, "Invalid character or faulted physics world");
    return nullptr;
  }
  const uint32_t index = static_cast<uint32_t>(handle) - 1;
  if (index >= world->characters.size() ||
      !world->characters[index].character ||
      world->characters[index].generation !=
          static_cast<uint32_t>(handle >> 32)) {
    fail(world, "Stale physics character handle");
    return nullptr;
  }
  return &world->characters[index];
}

static void character_read(VkrPhysicsWorld *world, const s_CharacterSlot &slot,
                           VkrPhysicsCharacterState *state) {
  const JPH::CharacterVirtual &character = *slot.character;
  *state = {};
  state->crouched = slot.crouched;
  store_vec(character.GetPosition(), state->foot_position);
  store_vec(character.GetLinearVelocity(), state->velocity);
  store_vec(character.GetGroundVelocity(), state->ground_velocity);
  /* A turning platform's spin, so what stands on it can turn with it. */
  const JPH::BodyID ground = character.GetGroundBodyID();
  if (!ground.IsInvalid()) {
    store_vec(world->system.GetBodyInterfaceNoLock().GetAngularVelocity(ground),
              state->ground_angular_velocity);
  }
  store_vec(character.GetGroundNormal(), state->ground_normal);
  state->ground_entity_id = character.GetGroundUserData();
  switch (character.GetGroundState()) {
  case JPH::CharacterBase::EGroundState::OnGround:
    state->ground = VKR_PHYSICS_CHARACTER_ON_GROUND;
    break;
  case JPH::CharacterBase::EGroundState::OnSteepGround:
    state->ground = VKR_PHYSICS_CHARACTER_STEEP_GROUND;
    break;
  case JPH::CharacterBase::EGroundState::NotSupported:
    state->ground = VKR_PHYSICS_CHARACTER_UNSUPPORTED;
    break;
  case JPH::CharacterBase::EGroundState::InAir:
    state->ground = VKR_PHYSICS_CHARACTER_IN_AIR;
    break;
  }
}

extern "C" VkrPhysicsCharacterDesc vkr_physics_character_default(void) {
  VkrPhysicsCharacterDesc desc{};
  desc.radius = 0.3f;
  desc.half_height = 0.6f;
  desc.max_slope_radians = 0.785398163f;
  desc.step_up = 0.35f;
  desc.step_down = 0.35f;
  desc.mass = 70.0f;
  desc.max_strength = 100.0f;
  desc.collision_layer = 1;
  desc.collision_mask = UINT16_MAX;
  return desc;
}

extern "C" bool8_t
vkr_physics_character_create(VkrPhysicsWorld *world,
                             const VkrPhysicsCharacterDesc *desc,
                             VkrPhysicsCharacter *out_character) {
  if (!world || world->faulted || world->dispatching ||
      world->reserved_destructions || !desc || !out_character ||
      !desc->entity_id || !finite_vector(desc->foot_position, 3) ||
      !std::isfinite(desc->radius) || desc->radius <= 0 || desc->radius > 100 ||
      !std::isfinite(desc->half_height) || desc->half_height <= 0 ||
      desc->half_height > 100 || !std::isfinite(desc->max_slope_radians) ||
      desc->max_slope_radians < 0 ||
      desc->max_slope_radians >= JPH::JPH_PI / 2 ||
      !std::isfinite(desc->step_up) || desc->step_up < 0 ||
      desc->step_up > 100 || !std::isfinite(desc->step_down) ||
      desc->step_down < 0 || desc->step_down > 100 ||
      !std::isfinite(desc->mass) || desc->mass <= 0 || desc->mass > 10000 ||
      !std::isfinite(desc->max_strength) || desc->max_strength < 0 ||
      desc->max_strength > 1.0e7f || !desc->collision_layer ||
      next_character_generation > UINT32_MAX) {
    return fail(world,
                "Invalid character settings, exhausted identity, or world");
  }
  uint32_t index = 0;
  while (index < world->characters.size() &&
         world->characters[index].character) {
    ++index;
  }
  if (index == world->characters.size()) {
    return fail(world, "Physics character capacity exceeded");
  }
  try {
    const uint32_t generation =
        static_cast<uint32_t>(next_character_generation);
    JPH::CharacterVirtualSettings settings;
    settings.mID = JPH::CharacterID(generation - 1);
    // Both shapes belong to this character slot and are released on
    // destruction. Stance transitions reuse them; only the capsule center
    // offset changes.
    JPH::RefConst<JPH::Shape> standing_shape =
        new JPH::CapsuleShape(desc->half_height, desc->radius);
    JPH::RefConst<JPH::Shape> crouched_shape = new JPH::CapsuleShape(
        desc->half_height * VKR_PHYSICS_CROUCH_HEIGHT_SHARE, desc->radius);
    settings.mShape = standing_shape;
    settings.mShapeOffset = JPH::Vec3(0, desc->half_height + desc->radius, 0);
    settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -desc->radius);
    settings.mMaxSlopeAngle = desc->max_slope_radians;
    settings.mMass = desc->mass;
    settings.mMaxStrength = desc->max_strength;
    settings.mMaxNumHits = VKR_PHYSICS_CHARACTER_MAX_HITS;
    settings.mBackFaceMode = JPH::EBackFaceMode::IgnoreBackFaces;
    JPH::Ref<JPH::CharacterVirtual> character = new JPH::CharacterVirtual(
        &settings, vec(desc->foot_position), JPH::Quat::sIdentity(),
        desc->entity_id, &world->system);
    const s_CharacterFilter filter(*desc);
    character->RefreshContacts({}, filter, filter, {}, world->scratch);
    if (character->GetMaxHitsExceeded()) {
      return fail(world, "Character initial contact capacity exceeded");
    }
    auto &slot = world->characters[index];
    slot.character = character;
    slot.standing_shape = standing_shape;
    slot.crouched_shape = crouched_shape;
    slot.desc = *desc;
    slot.generation = generation;
    slot.crouched = false_v;
    ++next_character_generation;
    *out_character = (static_cast<uint64_t>(generation) << 32) | (index + 1);
    return true_v;
  } catch (...) {
    return fail(world, "Character creation failed");
  }
}

extern "C" bool8_t vkr_physics_character_destroy(VkrPhysicsWorld *world,
                                                 VkrPhysicsCharacter handle) {
  if (world && (world->dispatching || world->reserved_destructions)) {
    return fail(world,
                "Character mutation during dispatch/prepared destruction");
  }
  auto *slot = character_lookup(world, handle);
  if (!slot) {
    return false_v;
  }
  slot->character = nullptr;
  slot->standing_shape = nullptr;
  slot->crouched_shape = nullptr;
  return true_v;
}

extern "C" bool8_t
vkr_physics_character_get_state(VkrPhysicsWorld *world,
                                VkrPhysicsCharacter handle,
                                VkrPhysicsCharacterState *state) {
  auto *slot = character_lookup(world, handle);
  if (!slot || !state) {
    return fail(world, "Invalid character state query");
  }
  slot->character->UpdateGroundVelocity();
  character_read(world, *slot, state);
  return true_v;
}

/* While a character stands on a moving body, its own move sees that body
   still; the step then carries it with the body (ride_position). */
class s_RideListener final : public JPH::CharacterContactListener {
public:
  explicit s_RideListener(JPH::BodyID ground) : ground(ground) {}
  void OnAdjustBodyVelocity(const JPH::CharacterVirtual *character,
                            const JPH::Body &body, JPH::Vec3 &linear,
                            JPH::Vec3 &angular) override {
    (void)character;
    if (body.GetID() == ground) {
      linear = JPH::Vec3::sZero();
      angular = JPH::Vec3::sZero();
    }
  }
  const JPH::BodyID ground;
};

/* The body a character on the ground stands on when it moves, or an
   invalid id: static ground carries nothing. */
static JPH::BodyID ride_ground(VkrPhysicsWorld *world,
                               const JPH::CharacterVirtual &character) {
  const JPH::BodyID ground = character.GetGroundBodyID();
  if (character.GetGroundState() !=
          JPH::CharacterBase::EGroundState::OnGround ||
      ground.IsInvalid()) {
    return JPH::BodyID();
  }
  JPH::BodyLockRead lock(world->system.GetBodyLockInterface(), ground);
  if (!lock.SucceededAndIsInBroadPhase() || lock.GetBody().IsStatic()) {
    return JPH::BodyID();
  }
  return ground;
}

/* Where `position` goes as body `ground` moves through the coming step of
   `dt`: turned about its center of mass by its angular velocity, then
   moved by its linear one, as the step integrates it. A kinematic body's
   velocities take it to the target its owner set this tick. */
static JPH::RVec3 ride_position(VkrPhysicsWorld *world, JPH::BodyID ground,
                                JPH::RVec3 position, float32_t dt) {
  JPH::BodyLockRead lock(world->system.GetBodyLockInterface(), ground);
  if (!lock.SucceededAndIsInBroadPhase()) {
    return position;
  }
  const JPH::Body &body = lock.GetBody();
  const JPH::RVec3 center = body.GetCenterOfMassPosition();
  JPH::Vec3 arm = JPH::Vec3(position - center);
  const JPH::Vec3 angular = body.GetAngularVelocity();
  const float32_t rate = angular.Length();
  if (rate > 1.0e-6f) {
    arm = JPH::Quat::sRotation(angular / rate, rate * dt) * arm;
  }
  return center + arm + body.GetLinearVelocity() * dt;
}

extern "C" bool8_t
vkr_physics_character_step(VkrPhysicsWorld *world, VkrPhysicsCharacter handle,
                           const VkrPhysicsCharacterInput *input,
                           VkrPhysicsCharacterState *state) {
  if (!world || world->dispatching || world->reserved_destructions || !input ||
      !state || !finite_vector(input->velocity, 3) ||
      !finite_vector(input->gravity, 3) || !std::isfinite(input->dt) ||
      input->dt <= 0 || input->dt > 0.1f) {
    return fail(world, "Invalid character step or mutation boundary");
  }
  auto *slot = character_lookup(world, handle);
  if (!slot) {
    return false_v;
  }
  try {
    JPH::CharacterVirtual::ExtendedUpdateSettings settings;
    settings.mWalkStairsStepUp = JPH::Vec3(0, slot->desc.step_up, 0);
    settings.mStickToFloorStepDown = JPH::Vec3(0, -slot->desc.step_down, 0);
    const s_CharacterFilter filter(slot->desc);
    const bool8_t crouch = input->crouch ? true_v : false_v;
    if (crouch != slot->crouched) {
      const JPH::Vec3 previous_offset = slot->character->GetShapeOffset();
      const float32_t half_height =
          slot->desc.half_height *
          (crouch ? VKR_PHYSICS_CROUCH_HEIGHT_SHARE : 1.0f);
      slot->character->SetShapeOffset(
          JPH::Vec3(0, half_height + slot->desc.radius, 0));
      const JPH::Shape *shape = crouch ? slot->crouched_shape.GetPtr()
                                       : slot->standing_shape.GetPtr();
      const float32_t penetration =
          1.5f * world->system.GetPhysicsSettings().mPenetrationSlop;
      if (slot->character->SetShape(shape, penetration, {}, filter, filter, {},
                                    world->scratch)) {
        slot->crouched = crouch;
      } else {
        // A ceiling is an ordinary rejected stance request, not a world fault.
        slot->character->SetShapeOffset(previous_offset);
      }
    }
    /* Standing on a moving body, the character moves as on still ground,
       relative to that body's pose before the step, and then the body's
       motion through the step carries it: up, down, along and around,
       where it stands when the step ends. */
    const JPH::BodyID ground = ride_ground(world, *slot->character);
    s_RideListener ride(ground);
    if (!ground.IsInvalid()) {
      slot->character->SetListener(&ride);
    }
    slot->character->SetLinearVelocity(vec(input->velocity) +
                                       input->dt * vec(input->gravity));
    /* Character and rigid updates are serialized and reuse fixed world scratch.
     * Jolt's retained contact vectors may grow within the 64-hit limit; this is
     * not a claim that the SDK performs no hot allocation. */
    slot->character->ExtendedUpdate(input->dt, vec(input->gravity), settings,
                                    {}, filter, filter, {}, world->scratch);
    slot->character->SetListener(nullptr);
    if (slot->character->GetMaxHitsExceeded()) {
      world->faulted = true_v;
      return fail(world, "Character contact capacity exceeded; reset required");
    }
    if (!ground.IsInvalid()) {
      const JPH::RVec3 from = slot->character->GetPosition();
      const JPH::RVec3 to = ride_position(world, ground, from, input->dt);
      slot->character->SetPosition(to);
      /* Velocities stay world-space: the ride adds to the solved move. */
      slot->character->SetLinearVelocity(slot->character->GetLinearVelocity() +
                                         JPH::Vec3(to - from) / input->dt);
      slot->character->UpdateGroundVelocity();
    }
    VkrPhysicsCharacterState result;
    character_read(world, *slot, &result);
    if (!finite_vector(result.foot_position, 3) ||
        !finite_vector(result.velocity, 3)) {
      world->faulted = true_v;
      return fail(world,
                  "Character left supported coordinates; reset required");
    }
    *state = result;
    return true_v;
  } catch (...) {
    slot->character->SetListener(nullptr);
    world->faulted = true_v;
    return fail(world, "Character update failed; reset required");
  }
}

static bool8_t shape_fail(const char **error, const char *message) {
  *error = message;
  return false_v;
}

/* Builds `c`'s shape without a world, so any thread can; failures name
   their reason in `error`. */
static bool8_t build_shape(const VkrPhysicsColliderDesc &c,
                           JPH::Ref<JPH::Shape> &shape, const char **error) {
  if (!finite_vector(c.scale, 3) || c.scale[0] <= 0 || c.scale[1] <= 0 ||
      c.scale[2] <= 0) {
    return shape_fail(error,
                      "Collision shape scale must be finite and positive");
  }
  JPH::Shape::ShapeResult result;
  switch (c.shape) {
  case VKR_PHYSICS_BOX:
    if (!finite_vector(c.half_extent, 3) || c.half_extent[0] <= 0 ||
        c.half_extent[1] <= 0 || c.half_extent[2] <= 0) {
      return shape_fail(error, "Box half extents must be positive");
    }
    result = JPH::BoxShapeSettings(vec(c.half_extent), 0.0f).Create();
    break;
  case VKR_PHYSICS_SPHERE:
  case VKR_PHYSICS_CAPSULE:
    if (!std::isfinite(c.radius) || c.radius <= 0 ||
        c.radius > VKR_PHYSICS_MAX_COORDINATE ||
        !std::isfinite(c.half_height) || c.half_height < 0 ||
        c.half_height > VKR_PHYSICS_MAX_COORDINATE ||
        c.scale[0] != c.scale[1] || c.scale[0] != c.scale[2]) {
      return shape_fail(
          error, "Round shapes require valid dimensions and uniform scale");
    }
    if (c.shape == VKR_PHYSICS_SPHERE || c.half_height == 0) {
      result = JPH::SphereShapeSettings(c.radius).Create();
    } else {
      result = JPH::CapsuleShapeSettings(c.half_height, c.radius).Create();
    }
    break;
  case VKR_PHYSICS_CONVEX_HULL:
  case VKR_PHYSICS_TRIANGLE_MESH: {
    const auto &g = c.geometry;
    if (!g.positions || g.vertex_count < 3 || g.vertex_count > 1048576 ||
        (c.shape == VKR_PHYSICS_CONVEX_HULL &&
         (g.vertex_count < 4 || g.vertex_count > 4096))) {
      return shape_fail(error, "Invalid collision geometry vertex count");
    }
    for (uint32_t i = 0; i < g.vertex_count; ++i) {
      if (!finite_vector(g.positions + i * 3, 3)) {
        return shape_fail(error, "Invalid collision geometry vertex");
      }
    }
    if (c.shape == VKR_PHYSICS_CONVEX_HULL) {
      JPH::ConvexHullShapeSettings settings;
      settings.mPoints.reserve(g.vertex_count);
      for (uint32_t i = 0; i < g.vertex_count; ++i) {
        settings.mPoints.push_back(vec(g.positions + i * 3));
      }
      result = settings.Create();
    } else {
      if (!g.indices || !g.index_count || g.index_count % 3 ||
          g.index_count > 3145728) {
        return shape_fail(error, "Invalid collision triangle index count");
      }
      JPH::MeshShapeSettings settings;
      settings.mTriangleVertices.reserve(g.vertex_count);
      settings.mIndexedTriangles.reserve(g.index_count / 3);
      for (uint32_t i = 0; i < g.vertex_count; ++i) {
        settings.mTriangleVertices.emplace_back(
            g.positions[i * 3], g.positions[i * 3 + 1], g.positions[i * 3 + 2]);
      }
      for (uint32_t i = 0; i < g.index_count; i += 3) {
        uint32_t a = g.indices[i];
        uint32_t b = g.indices[i + 1];
        uint32_t c_index = g.indices[i + 2];
        if (a >= g.vertex_count || b >= g.vertex_count ||
            c_index >= g.vertex_count || a == b || a == c_index ||
            b == c_index ||
            (vec(g.positions + b * 3) - vec(g.positions + a * 3))
                    .Cross(vec(g.positions + c_index * 3) -
                           vec(g.positions + a * 3))
                    .LengthSq() <= 1.0e-16f) {
          return shape_fail(
              error, "Collision mesh has invalid or degenerate triangle");
        }
        settings.mIndexedTriangles.emplace_back(a, b, c_index, 0);
      }
      // Jolt refuses a whole mesh for one sliver it judges degenerate, by
      // area or after quantizing to the mesh bounds; such a triangle has no
      // area to collide with, so it leaves and the rest of the mesh stays.
      settings.Sanitize();
      result = settings.Create();
    }
    break;
  }
  case VKR_PHYSICS_HEIGHT_FIELD: {
    const auto &g = c.geometry;
    const uint32_t samples = g.height_samples;
    if (!g.positions || samples < 4 || samples > 8192 || samples % 4 ||
        g.vertex_count != samples * samples ||
        !std::isfinite(g.height_spacing) || g.height_spacing <= 0) {
      return shape_fail(error, "Invalid height field size or spacing");
    }
    for (uint32_t i = 0; i < g.vertex_count; ++i) {
      if (!std::isfinite(g.positions[i]) &&
          g.positions[i] != VKR_PHYSICS_HEIGHT_HOLE) {
        return shape_fail(error, "Invalid height field sample");
      }
    }
    JPH::HeightFieldShapeSettings settings(
        g.positions, JPH::Vec3::sZero(),
        JPH::Vec3(g.height_spacing, 1.0f, g.height_spacing), samples);
    settings.mBlockSize = 4;
    result = settings.Create();
    break;
  }
  default:
    return shape_fail(error, "Unknown collision shape");
  }
  if (result.HasError()) {
    log_warn("Physics: Jolt rejected a collision shape: %s",
             result.GetError().c_str());
    return shape_fail(error, "Jolt rejected collision shape");
  }
  shape = result.Get();
  shape->SetUserData(c.entity_id);
  if (!shape->IsValidScale(vec(c.scale))) {
    return shape_fail(error,
                      "Collision shape does not support requested scale");
  }
  const auto bounds = shape->GetLocalBounds();
  for (uint32_t axis = 0; axis < 3; ++axis) {
    if (std::max(std::abs(bounds.mMin[axis]), std::abs(bounds.mMax[axis])) *
            c.scale[axis] >
        VKR_PHYSICS_MAX_COORDINATE) {
      return shape_fail(error,
                        "Scaled collision shape exceeds supported coordinates");
    }
  }
  if (c.scale[0] != 1 || c.scale[1] != 1 || c.scale[2] != 1) {
    auto scaled = JPH::ScaledShapeSettings(shape, vec(c.scale)).Create();
    if (scaled.HasError()) {
      return shape_fail(error, "Jolt rejected scaled collision shape");
    }
    shape = scaled.Get();
  }
  return true_v;
}

/* A collider's shape built on a worker (vkr_physics_shape_build_begin). The
   owner and a queued job each hold a reference; the last to let go frees
   it. */
struct s_VkrPhysicsShapeBuild {
  VkrPhysicsColliderDesc collider;
  std::vector<float32_t> positions;
  std::vector<uint32_t> indices;
  JPH::Ref<JPH::Shape> shape;
  const char *error = "";
  std::atomic<uint32_t> references{1};
  std::atomic<bool> done{false};
};

static void shape_build_run(VkrPhysicsShapeBuild *build) {
  try {
    if (!build_shape(build->collider, build->shape, &build->error)) {
      build->shape = nullptr;
    }
  } catch (...) {
    build->shape = nullptr;
    build->error = "Physics shape allocation failed";
  }
  build->done.store(true, std::memory_order_release);
}

static void shape_build_drop(VkrPhysicsShapeBuild *build) {
  if (build->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    delete build;
  }
}

static bool8_t create_shape(VkrPhysicsWorld *world,
                            const VkrPhysicsColliderDesc &c,
                            JPH::Ref<JPH::Shape> &shape) {
  const VkrPhysicsShapeBuild *build = c.prebuilt;
  if (build) {
    if (!build->done.load(std::memory_order_acquire) ||
        build->collider.entity_id != c.entity_id ||
        build->collider.shape != c.shape) {
      return fail(world, "Prebuilt collision shape is unfinished or foreign");
    }
    if (!build->shape) {
      return fail(world, build->error);
    }
    shape = build->shape;
    return true_v;
  }
  const char *error = "";
  if (!build_shape(c, shape, &error)) {
    return fail(world, error);
  }
  return true_v;
}

extern "C" bool8_t vkr_physics_body_create(VkrPhysicsWorld *world,
                                           const VkrPhysicsBodyDesc *desc,
                                           VkrPhysicsBody *out_body) {
  if (world && world->dispatching) {
    return fail(world, "Physics mutation or drain during event dispatch");
  }

  if (out_body) {
    *out_body = VKR_PHYSICS_BODY_INVALID;
  }
  if (!world || world->faulted || !desc || !out_body || !desc->colliders ||
      desc->entity_id == 0 || desc->collider_count == 0 ||
      desc->collider_count > VKR_PHYSICS_MAX_COLLIDERS ||
      desc->motion < VKR_PHYSICS_STATIC || desc->motion > VKR_PHYSICS_DYNAMIC ||
      !finite_vector(desc->position, 3) || !valid_rotation(desc->rotation) ||
      !std::isfinite(desc->mass) || desc->mass <= 0 ||
      !std::isfinite(desc->friction) || desc->friction < 0 ||
      desc->friction > 1 || !std::isfinite(desc->restitution) ||
      desc->restitution < 0 || desc->restitution > 1 ||
      !std::isfinite(desc->gravity_factor) ||
      std::abs(desc->gravity_factor) > 100 ||
      !std::isfinite(desc->linear_damping) || desc->linear_damping < 0 ||
      desc->linear_damping > 1 || !std::isfinite(desc->angular_damping) ||
      desc->angular_damping < 0 || desc->angular_damping > 1) {
    return fail(world, "Invalid physics body description");
  }
  for (const auto &slot : world->slots) {
    if (desc->enabled && slot.occupied && slot.enabled &&
        world->system.GetBodyInterface().GetUserData(slot.id) ==
            desc->entity_id) {
      return fail(world, "Duplicate physics body entity ID");
    }
  }
  uint32_t index = 0;
  while (index < world->slots.size() && world->slots[index].occupied) {
    ++index;
  }
  if (next_body_generation > UINT32_MAX) {
    return fail(world, "Physics handle generation exhausted");
  }
  if (index == world->slots.size()) {
    return fail(world, "Physics body capacity exceeded");
  }
  try {
    JPH::StaticCompoundShapeSettings compound;
    uint32_t enabled_count = 0;
    for (uint32_t i = 0; i < desc->collider_count; ++i) {
      const auto &c = desc->colliders[i];
      if (c.entity_id == 0) {
        return fail(world, "Collider entity ID must be valid");
      }
      for (uint32_t previous = 0; previous < i; ++previous) {
        if (desc->colliders[previous].entity_id == c.entity_id) {
          return fail(world, "Duplicate collider entity ID");
        }
      }
      if (!c.enabled) {
        continue;
      }
      if (!finite_vector(c.position, 3) || !valid_rotation(c.rotation)) {
        return fail(world, "Invalid collider transform");
      }
      JPH::Ref<JPH::Shape> shape;
      if ((c.shape == VKR_PHYSICS_TRIANGLE_MESH ||
           c.shape == VKR_PHYSICS_HEIGHT_FIELD) &&
          desc->motion == VKR_PHYSICS_DYNAMIC) {
        return fail(
            world,
            "Triangle mesh collision supports static/kinematic bodies only");
      }
      if (!create_shape(world, c, shape)) {
        return false_v;
      }
      compound.AddShape(vec(c.position), quat(c.rotation), shape);
      ++enabled_count;
    }
    if (!enabled_count) {
      return fail(world, "Physics body needs an enabled collider");
    }
    auto shape_result = compound.Create();
    if (shape_result.HasError()) {
      return fail(world, "Jolt rejected compound shape");
    }
    auto motion = static_cast<JPH::EMotionType>(desc->motion);
    JPH::BodyCreationSettings settings(
        shape_result.Get(), vec(desc->position), quat(desc->rotation), motion,
        static_cast<uint32_t>(desc->collision_layer) |
            (static_cast<uint32_t>(desc->collision_mask) << 16));
    settings.mUserData = desc->entity_id;
    settings.mFriction = desc->friction;
    settings.mRestitution = desc->restitution;
    settings.mGravityFactor = desc->gravity_factor;
    settings.mLinearDamping = desc->linear_damping;
    settings.mAngularDamping = desc->angular_damping;
    settings.mAllowSleeping = desc->allow_sleep != 0;
    settings.mIsSensor = desc->sensor != 0;
    settings.mUseManifoldReduction = false;
    settings.mMotionQuality = desc->continuous ? JPH::EMotionQuality::LinearCast
                                               : JPH::EMotionQuality::Discrete;
    settings.mOverrideMassProperties =
        JPH::EOverrideMassProperties::CalculateInertia;
    settings.mMassPropertiesOverride.mMass = desc->mass;
    if (desc->motion != VKR_PHYSICS_DYNAMIC) {
      settings.mOverrideMassProperties =
          JPH::EOverrideMassProperties::MassAndInertiaProvided;
      settings.mMassPropertiesOverride.mInertia = JPH::Mat44::sIdentity();
    }
    auto &bodies = world->system.GetBodyInterface();
    auto *body = bodies.CreateBody(settings);
    if (!body) {
      return fail(world, "Jolt body capacity exceeded");
    }
    auto &slot = world->slots[index];
    slot.generation = static_cast<uint32_t>(next_body_generation++);
    slot.id = body->GetID();
    world->native_to_slot[slot.id.GetIndex()] = index;
    slot.occupied = true_v;
    slot.enabled = desc->enabled != 0;
    slot.motion = desc->motion;
    slot.sensor = desc->sensor;
    slot.disabled_linear = JPH::Vec3::sZero();
    slot.disabled_angular = JPH::Vec3::sZero();
    if (slot.enabled) {
      bodies.AddBody(slot.id, desc->motion == VKR_PHYSICS_STATIC
                                  ? JPH::EActivation::DontActivate
                                  : JPH::EActivation::Activate);
    }
    *out_body = handle_for(world, index);
    return true_v;
  } catch (...) {
    return fail(world, "Physics body allocation failed");
  }
}

static bool8_t update_sensors(VkrPhysicsWorld *world);
static bool8_t update_contacts(VkrPhysicsWorld *world);
static bool8_t end_contact_pairs(VkrPhysicsWorld *world, VkrPhysicsBody body);
static bool8_t end_sensor_pairs(VkrPhysicsWorld *world, uint64_t entity_id);

extern "C" bool8_t vkr_physics_body_destroy(VkrPhysicsWorld *world,
                                            VkrPhysicsBody body) {
  if (world && world->dispatching) {
    return fail(world, "Physics mutation or drain during event dispatch");
  }

  auto *slot = lookup(world, body);
  if (!slot) {
    return false_v;
  }
  auto &bodies = world->system.GetBodyInterface();
  if (slot->destroy_reserved) {
    world->reserved_events -= slot->reserved_events;
    world->reserved_contact_events -= slot->reserved_contacts;
    --world->reserved_destructions;
    slot->destroy_reserved = false_v;
    slot->reserved_events = 0;
    slot->reserved_contacts = 0;
  }
  const bool was_enabled = slot->enabled != 0;
  if (was_enabled && (!end_sensor_pairs(world, bodies.GetUserData(slot->id)) ||
                      !end_contact_pairs(world, body))) {
    return false_v;
  }
  if (slot->enabled) {
    bodies.RemoveBody(slot->id);
  }
  remove_attached_joints(world, body);
  world->native_to_slot[slot->id.GetIndex()] = UINT32_MAX;
  bodies.DestroyBody(slot->id);
  slot->occupied = false_v;
  slot->generation = 0;
  // Removing support must wake sleepers; deletion is a cold boundary.
  for (const auto &other : world->slots) {
    if (was_enabled && other.occupied && other.enabled &&
        other.motion == VKR_PHYSICS_DYNAMIC) {
      bodies.ActivateBody(other.id);
    }
  }
  return true_v;
}

extern "C" bool8_t vkr_physics_body_set_enabled(VkrPhysicsWorld *world,
                                                VkrPhysicsBody body,
                                                bool8_t enabled) {
  if (world && world->dispatching) {
    return fail(world, "Physics mutation or drain during event dispatch");
  }

  auto *slot = lookup(world, body);
  if (!slot) {
    return false_v;
  }
  enabled = enabled != 0;
  if (slot->destroy_reserved) {
    if (enabled) {
      return fail(world, "Body has a prepared destruction");
    }
    world->reserved_events -= slot->reserved_events;
    world->reserved_contact_events -= slot->reserved_contacts;
    --world->reserved_destructions;
    slot->destroy_reserved = false_v;
    slot->reserved_events = 0;
    slot->reserved_contacts = 0;
  }
  if (slot->enabled == enabled) {
    return true_v;
  }
  auto &bodies = world->system.GetBodyInterface();
  if (enabled) {
    const uint64_t entity_id = bodies.GetUserData(slot->id);
    for (const auto &other : world->slots) {
      if (&other != slot && other.occupied && other.enabled &&
          bodies.GetUserData(other.id) == entity_id) {
        return fail(world, "Duplicate enabled physics body entity ID");
      }
    }
    bodies.AddBody(slot->id, slot->motion == VKR_PHYSICS_STATIC
                                 ? JPH::EActivation::DontActivate
                                 : JPH::EActivation::Activate);
    if (slot->motion != VKR_PHYSICS_STATIC) {
      bodies.SetLinearAndAngularVelocity(slot->id, slot->disabled_linear,
                                         slot->disabled_angular);
    }
  } else {
    if (!end_sensor_pairs(world, bodies.GetUserData(slot->id)) ||
        !end_contact_pairs(world, body)) {
      return false_v;
    }
    // Jolt deactivation zeros velocity. Disabled VKR bodies retain it for
    // testing and restore it only after rejoining the broad phase.
    bodies.GetLinearAndAngularVelocity(slot->id, slot->disabled_linear,
                                       slot->disabled_angular);
    bodies.RemoveBody(slot->id);
    for (const auto &other : world->slots) {
      if (&other != slot && other.occupied && other.enabled &&
          other.motion == VKR_PHYSICS_DYNAMIC) {
        bodies.ActivateBody(other.id);
      }
    }
  }
  slot->enabled = enabled;
  refresh_attached_joints(world, body);
  return true_v;
}

extern "C" bool8_t vkr_physics_world_shift(VkrPhysicsWorld *world,
                                           const float32_t shift[3]) {
  if (!world || world->dispatching || !finite_vector(shift, 3)) {
    return fail(world, "Invalid origin shift");
  }
  const JPH::Vec3 offset = vec(shift);
  auto &bodies = world->system.GetBodyInterface();
  for (const auto &slot : world->slots) {
    if (slot.occupied) {
      bodies.SetPosition(slot.id, bodies.GetPosition(slot.id) - offset,
                         JPH::EActivation::DontActivate);
    }
  }
  for (auto &slot : world->characters) {
    if (slot.character) {
      slot.character->SetPosition(slot.character->GetPosition() - offset);
    }
  }
  world->system.OptimizeBroadPhase();
  return true_v;
}

extern "C" bool8_t vkr_physics_body_set_pose(VkrPhysicsWorld *world,
                                             VkrPhysicsBody body,
                                             const float32_t position[3],
                                             const float32_t rotation[4]) {
  if (world && world->dispatching) {
    return fail(world, "Physics mutation or drain during event dispatch");
  }

  auto *slot = lookup(world, body);
  if (!slot || slot->destroy_reserved || !finite_vector(position, 3) ||
      !valid_rotation(rotation)) {
    return fail(world, "Invalid body or pose");
  }
  world->system.GetBodyInterface().SetPositionAndRotation(
      slot->id, vec(position), quat(rotation),
      slot->enabled && slot->motion != VKR_PHYSICS_STATIC
          ? JPH::EActivation::Activate
          : JPH::EActivation::DontActivate);
  return true_v;
}

extern "C" bool8_t vkr_physics_body_move_kinematic(VkrPhysicsWorld *world,
                                                   VkrPhysicsBody body,
                                                   const float32_t position[3],
                                                   const float32_t rotation[4],
                                                   float32_t dt) {
  if (world && world->dispatching) {
    return fail(world, "Physics mutation or drain during event dispatch");
  }

  auto *slot = lookup(world, body);
  if (!slot || slot->destroy_reserved ||
      slot->motion != VKR_PHYSICS_KINEMATIC || !slot->enabled ||
      !finite_vector(position, 3) || !valid_rotation(rotation) ||
      !std::isfinite(dt) || dt <= 0 || dt > 0.1f) {
    return fail(world, "Invalid kinematic target");
  }
  world->system.GetBodyInterface().MoveKinematic(slot->id, vec(position),
                                                 quat(rotation), dt);
  return true_v;
}

extern "C" bool8_t vkr_physics_body_impulse(VkrPhysicsWorld *world,
                                            VkrPhysicsBody body,
                                            const float32_t impulse[3],
                                            const float32_t world_point[3]) {
  if (world && world->dispatching) {
    return fail(world, "Physics mutation or drain during event dispatch");
  }

  auto *slot = lookup(world, body);
  if (!slot || slot->destroy_reserved || slot->motion != VKR_PHYSICS_DYNAMIC ||
      !slot->enabled || !finite_vector(impulse, 3) ||
      (world_point && !finite_vector(world_point, 3))) {
    return fail(world, "Invalid dynamic body or impulse");
  }
  auto &bodies = world->system.GetBodyInterface();
  if (world_point) {
    bodies.AddImpulse(slot->id, vec(impulse), vec(world_point));
  } else {
    bodies.AddImpulse(slot->id, vec(impulse));
  }
  return true_v;
}

extern "C" bool8_t vkr_physics_body_get_pose(VkrPhysicsWorld *world,
                                             VkrPhysicsBody body,
                                             VkrPhysicsPose *out_pose) {
  auto *slot = lookup(world, body);
  if (!slot || !out_pose) {
    return fail(world, "Invalid body or pose output");
  }
  auto &bodies = world->system.GetBodyInterface();
  JPH::RVec3 position;
  JPH::Quat rotation;
  JPH::Vec3 linear;
  JPH::Vec3 angular;
  bodies.GetPositionAndRotation(slot->id, position, rotation);
  if (slot->enabled) {
    bodies.GetLinearAndAngularVelocity(slot->id, linear, angular);
  } else {
    linear = slot->disabled_linear;
    angular = slot->disabled_angular;
  }
  store_vec(position, out_pose->position);
  out_pose->rotation[0] = rotation.GetX();
  out_pose->rotation[1] = rotation.GetY();
  out_pose->rotation[2] = rotation.GetZ();
  out_pose->rotation[3] = rotation.GetW();
  store_vec(linear, out_pose->linear_velocity);
  store_vec(angular, out_pose->angular_velocity);
  out_pose->active = bodies.IsActive(slot->id);
  return true_v;
}

extern "C" bool8_t vkr_physics_world_set_gravity(VkrPhysicsWorld *world,
                                                 const float32_t gravity[3]) {
  if (world && world->dispatching) {
    return fail(world, "Physics mutation or drain during event dispatch");
  }
  if (!world || !gravity || !finite_vector(gravity, 3) ||
      std::abs(gravity[0]) > 1000.0f || std::abs(gravity[1]) > 1000.0f ||
      std::abs(gravity[2]) > 1000.0f) {
    return fail(world, "Gravity must be finite and within 1000 m/s^2");
  }
  try {
    const JPH::Vec3 value(gravity[0], gravity[1], gravity[2]);
    if (world->system.GetGravity() == value) {
      return true_v;
    }
    world->system.SetGravity(value);
    /* A resting body keeps sleeping under a new gravity unless woken. */
    JPH::BodyIDVector bodies;
    world->system.GetBodies(bodies);
    world->system.GetBodyInterface().ActivateBodies(bodies.data(),
                                                    (int)bodies.size());
    return true_v;
  } catch (...) {
    return fail(world, "Setting physics gravity failed");
  }
}

extern "C" void vkr_physics_set_jobs(const VkrPhysicsJobs *jobs) {
  if (s_jobs) {
    while (s_jobs->outstanding.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    delete s_jobs;
    s_jobs = nullptr;
  }
  if (jobs && jobs->submit && jobs->worker_count) {
    // Jolt allocates through its registered functions, as worlds do.
    JPH::RegisterDefaultAllocator();
    s_jobs = new s_JobSystem(*jobs);
  }
}

static void shape_build_job(void *arg) {
  auto *build = static_cast<VkrPhysicsShapeBuild *>(arg);
  /* Clearing the jobs waits for this one, so the job system outlives it. */
  s_JobSystem *jobs = s_jobs;
  shape_build_run(build);
  shape_build_drop(build);
  jobs->task_done();
}

extern "C" VkrPhysicsShapeBuild *
vkr_physics_shape_build_begin(const VkrPhysicsColliderDesc *collider) {
  if (!collider) {
    return nullptr;
  }
  // A build may be the first Jolt allocation, before any world.
  if (!JPH::Allocate) {
    JPH::RegisterDefaultAllocator();
  }
  VkrPhysicsShapeBuild *build = nullptr;
  try {
    build = new VkrPhysicsShapeBuild();
    build->collider = *collider;
    build->collider.prebuilt = nullptr;
    const VkrPhysicsGeometry &geometry = collider->geometry;
    if (geometry.positions && geometry.vertex_count) {
      const size_t floats =
          collider->shape == VKR_PHYSICS_HEIGHT_FIELD
              ? geometry.vertex_count
              : 3u * static_cast<size_t>(geometry.vertex_count);
      build->positions.assign(geometry.positions, geometry.positions + floats);
      build->collider.geometry.positions = build->positions.data();
    }
    if (geometry.indices && geometry.index_count) {
      build->indices.assign(geometry.indices,
                            geometry.indices + geometry.index_count);
      build->collider.geometry.indices = build->indices.data();
    }
  } catch (...) {
    delete build;
    return nullptr;
  }
  build->references.store(2, std::memory_order_relaxed);
  if (!s_jobs || !s_jobs->submit_task(shape_build_job, build)) {
    build->references.store(1, std::memory_order_relaxed);
    shape_build_run(build);
  }
  return build;
}

extern "C" bool8_t
vkr_physics_shape_build_done(const VkrPhysicsShapeBuild *build) {
  return build && build->done.load(std::memory_order_acquire);
}

extern "C" void vkr_physics_shape_build_release(VkrPhysicsShapeBuild *build) {
  if (build) {
    shape_build_drop(build);
  }
}

extern "C" bool8_t vkr_physics_step(VkrPhysicsWorld *world, float32_t dt) {
  if (world && world->dispatching) {
    return fail(world, "Physics mutation or drain during event dispatch");
  }

  if (!world || world->faulted || world->reserved_destructions ||
      !std::isfinite(dt) || dt <= 0 || dt > 0.1f) {
    return fail(world, "Invalid physics timestep or faulted world");
  }
  try {
    world->next_contacts.clear();
    if (++world->contact_stamp == 0) {
      std::fill(world->contact_stamps.begin(), world->contact_stamps.end(), 0);
      world->contact_stamp = 1;
    }
    // Few bodies finish faster on this thread than spread over workers.
    JPH::JobSystem *jobs = &world->jobs;
    if (s_jobs && world->system.GetNumActiveBodies(JPH::EBodyType::RigidBody) >=
                      s_jobs->min_active_bodies()) {
      jobs = s_jobs;
    }
    if (world->system.Update(dt, 1, &world->scratch, jobs) !=
        JPH::EPhysicsUpdateError::None) {
      world->faulted = true_v;
      return fail(world, "Physics contact capacity exceeded; reset required");
    }
    std::sort(world->next_contacts.begin(), world->next_contacts.end(),
              contact_before);
    return update_contacts(world) && update_sensors(world);
  } catch (...) {
    world->faulted = true_v;
    return fail(world, "Physics update failed; reset required");
  }
}

class s_QueryFilter final : public JPH::ObjectLayerFilter {
public:
  explicit s_QueryFilter(uint16_t mask) : mask(mask) {}
  bool ShouldCollide(JPH::ObjectLayer layer) const override {
    return (layer & mask) != 0;
  }
  uint16_t mask;
};

static bool valid_query_filter(const VkrPhysicsQueryFilter *filter) {
  return !filter || (filter->ignored_count <= VKR_PHYSICS_MAX_QUERY_IGNORES &&
                     (!filter->ignored_count || filter->ignored_entities));
}

class s_QueryBodyFilter final : public JPH::BodyFilter {
public:
  explicit s_QueryBodyFilter(const VkrPhysicsQueryFilter *filter)
      : filter(filter) {}
  bool ShouldCollideLocked(const JPH::Body &body) const override {
    if (!filter) {
      return true;
    }
    if (!filter->include_sensors && body.IsSensor()) {
      return false;
    }
    for (uint32_t i = 0; i < filter->ignored_count; ++i) {
      if (filter->ignored_entities[i] == body.GetUserData()) {
        return false;
      }
    }
    return true;
  }
  const VkrPhysicsQueryFilter *filter;
};

extern "C" bool8_t
vkr_physics_raycast_query(VkrPhysicsWorld *world, const float32_t origin[3],
                          const float32_t displacement[3],
                          const VkrPhysicsQueryFilter *query_filter,
                          VkrPhysicsRayHit *out_hit) {
  if (!world || world->faulted || !out_hit || !finite_vector(origin, 3) ||
      !finite_vector(displacement, 3) || !valid_query_filter(query_filter)) {
    return fail(world, "Invalid ray query or faulted world");
  }
  *out_hit = {};
  JPH::RRayCast ray(vec(origin), vec(displacement));
  JPH::RayCastResult result;
  s_QueryFilter filter(query_filter ? query_filter->mask : UINT16_MAX);
  s_QueryBodyFilter bodies(query_filter);
  if (!world->system.GetNarrowPhaseQuery().CastRay(ray, result, {}, filter,
                                                   bodies)) {
    return false_v;
  }
  JPH::BodyLockRead lock(world->system.GetBodyLockInterface(), result.mBodyID);
  if (!lock.Succeeded()) {
    return fail(world, "Ray hit lost body");
  }
  const auto &body = lock.GetBody();
  for (uint32_t i = 0; i < world->slots.size(); ++i) {
    if (world->slots[i].occupied && world->slots[i].id == result.mBodyID) {
      out_hit->body = handle_for(world, i);
      break;
    }
  }
  out_hit->entity_id = body.GetUserData();
  out_hit->collider_entity_id =
      body.GetShape()->GetSubShapeUserData(result.mSubShapeID2);
  out_hit->fraction = result.mFraction;
  auto point = ray.GetPointOnRay(result.mFraction);
  store_vec(point, out_hit->position);
  store_vec(body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point),
            out_hit->normal);
  return true_v;
}

extern "C" bool8_t vkr_physics_raycast(VkrPhysicsWorld *world,
                                       const float32_t origin[3],
                                       const float32_t displacement[3],
                                       VkrPhysicsRayHit *out_hit) {
  return vkr_physics_raycast_filtered(world, origin, displacement, UINT16_MAX,
                                      out_hit);
}

static VkrPhysicsBody body_handle(VkrPhysicsWorld *world, JPH::BodyID id) {
  if (id.GetIndex() >= world->native_to_slot.size()) {
    return VKR_PHYSICS_BODY_INVALID;
  }
  uint32_t index = world->native_to_slot[id.GetIndex()];
  if (index == UINT32_MAX || !world->slots[index].occupied ||
      world->slots[index].id != id) {
    return VKR_PHYSICS_BODY_INVALID;
  }
  return handle_for(world, index);
}

class s_OverlapCollector final : public JPH::CollideShapeCollector {
public:
  s_OverlapCollector(VkrPhysicsWorld *world, VkrPhysicsOverlapHit *hits,
                     uint32_t capacity)
      : world(world), hits(hits), capacity(capacity) {}
  void AddHit(const JPH::CollideShapeResult &hit) override {
    auto &bodies = world->system.GetBodyInterfaceNoLock();
    uint64_t child =
        bodies.GetShape(hit.mBodyID2)->GetSubShapeUserData(hit.mSubShapeID2);
    VkrPhysicsBody handle = body_handle(world, hit.mBodyID2);
    for (uint32_t i = 0; i < count; ++i) {
      if (hits[i].body == handle && hits[i].collider_entity_id == child) {
        return;
      }
    }
    if (count == capacity) {
      overflow = true;
      ForceEarlyOut();
      return;
    }
    hits[count++] = {handle, bodies.GetUserData(hit.mBodyID2), child};
  }
  VkrPhysicsWorld *world;
  VkrPhysicsOverlapHit *hits;
  uint32_t capacity;
  uint32_t count = 0;
  bool overflow = false;
};

extern "C" bool8_t
vkr_physics_overlap_sphere(VkrPhysicsWorld *world, const float32_t center[3],
                           float32_t radius, uint16_t query_mask,
                           VkrPhysicsOverlapHit *hits, uint32_t capacity,
                           uint32_t *out_count) {
  if (out_count) {
    *out_count = 0;
  }
  if (!world || world->faulted || !out_count || (!hits && capacity) ||
      !finite_vector(center, 3) || !std::isfinite(radius) || radius <= 0 ||
      radius > 1.0e7f) {
    return fail(world, "Invalid sphere overlap query");
  }
  JPH::SphereShape sphere(radius);
  s_QueryFilter filter(query_mask);
  s_OverlapCollector collector(world, hits, capacity);
  world->system.GetNarrowPhaseQuery().CollideShape(
      &sphere, JPH::Vec3::sReplicate(1), JPH::RMat44::sTranslation(vec(center)),
      JPH::CollideShapeSettings(), vec(center), collector, {}, filter);
  if (collector.overflow) {
    return fail(world, "Sphere overlap output capacity exceeded");
  }
  *out_count = collector.count;
  return true_v;
}

static bool same_pair(const VkrPhysicsSensorEvent &a,
                      const VkrPhysicsSensorEvent &b) {
  return a.entity_a == b.entity_a && a.collider_a == b.collider_a &&
         a.entity_b == b.entity_b && a.collider_b == b.collider_b;
}

static bool8_t event_fault(VkrPhysicsWorld *world) {
  world->faulted = true_v;
  world->events.clear();
  world->contact_events.clear();
  return fail(world,
              "Physics contact/sensor/event capacity exceeded; reset required");
}

static bool8_t queue_event(VkrPhysicsWorld *world, VkrPhysicsSensorEvent event,
                           bool8_t began) {
  if (world->events.size() + world->reserved_events >=
      world->events.capacity()) {
    return event_fault(world);
  }
  event.began = began;
  world->events.push_back(event);
  return true_v;
}

static bool8_t end_sensor_pairs(VkrPhysicsWorld *world, uint64_t entity_id) {
  for (size_t i = 0; i < world->sensor_pairs.size();) {
    const auto &pair = world->sensor_pairs[i];
    if (pair.entity_a == entity_id || pair.entity_b == entity_id) {
      if (!queue_event(world, pair, false_v)) {
        return false_v;
      }
      world->sensor_pairs[i] = world->sensor_pairs.back();
      world->sensor_pairs.pop_back();
    } else {
      ++i;
    }
  }
  return true_v;
}

class s_SensorBodyFilter final : public JPH::BodyFilter {
public:
  explicit s_SensorBodyFilter(JPH::BodyID excluded) : excluded(excluded) {}
  bool ShouldCollide(const JPH::BodyID &id) const override {
    return id != excluded;
  }
  JPH::BodyID excluded;
};

/* Records one sensor overlap for this update, ordered by entity and
   collider so either side reports the same pair. False when the pair
   storage is full, which faults the world. */
static bool sensor_pair_add(VkrPhysicsWorld *world,
                            VkrPhysicsSensorEvent pair) {
  if (pair.entity_a > pair.entity_b ||
      (pair.entity_a == pair.entity_b && pair.collider_a > pair.collider_b)) {
    std::swap(pair.entity_a, pair.entity_b);
    std::swap(pair.collider_a, pair.collider_b);
  }
  for (const auto &existing : world->next_pairs) {
    if (same_pair(existing, pair)) {
      return true;
    }
  }
  if (world->next_pairs.size() == world->next_pairs.capacity()) {
    event_fault(world);
    return false;
  }
  world->next_pairs.push_back(pair);
  return true;
}

class s_SensorCollector final : public JPH::CollideShapeCollector {
public:
  s_SensorCollector(VkrPhysicsWorld *world, JPH::BodyID sensor)
      : world(world), sensor(sensor) {}
  void AddHit(const JPH::CollideShapeResult &hit) override {
    auto &bodies = world->system.GetBodyInterfaceNoLock();
    auto layer_a = bodies.GetObjectLayer(sensor);
    auto layer_b = bodies.GetObjectLayer(hit.mBodyID2);
    if (!world->pair_filter.ShouldCollide(layer_a, layer_b)) {
      return;
    }
    const VkrPhysicsSensorEvent pair = {
        bodies.GetUserData(sensor),
        bodies.GetShape(sensor)->GetSubShapeUserData(hit.mSubShapeID1),
        bodies.GetUserData(hit.mBodyID2),
        bodies.GetShape(hit.mBodyID2)->GetSubShapeUserData(hit.mSubShapeID2),
        false_v};
    if (!sensor_pair_add(world, pair)) {
      ForceEarlyOut();
    }
  }
  VkrPhysicsWorld *world;
  JPH::BodyID sensor;
};

/* Characters have no body for the narrow phase to find, so each sensor
   tests their capsules directly. A character pair names its entity with
   collider zero. */
static bool8_t sensor_characters(VkrPhysicsWorld *world, JPH::BodyID sensor) {
  auto &bodies = world->system.GetBodyInterfaceNoLock();
  const JPH::ObjectLayer sensor_layer = bodies.GetObjectLayer(sensor);
  const JPH::RefConst<JPH::Shape> sensor_shape = bodies.GetShape(sensor);
  const JPH::Mat44 sensor_transform = bodies.GetCenterOfMassTransform(sensor);
  for (const auto &slot : world->characters) {
    if (!slot.character) {
      continue;
    }
    const JPH::ObjectLayer character_layer = static_cast<JPH::ObjectLayer>(
        slot.desc.collision_layer |
        (static_cast<uint32_t>(slot.desc.collision_mask) << 16));
    if (bodies.GetUserData(sensor) == slot.desc.entity_id ||
        !world->pair_filter.ShouldCollide(sensor_layer, character_layer)) {
      continue;
    }
    JPH::AnyHitCollisionCollector<JPH::CollideShapeCollector> hit;
    JPH::CollisionDispatch::sCollideShapeVsShape(
        sensor_shape, slot.character->GetShape(), JPH::Vec3::sReplicate(1),
        JPH::Vec3::sReplicate(1), sensor_transform,
        slot.character->GetCenterOfMassTransform(), JPH::SubShapeIDCreator(),
        JPH::SubShapeIDCreator(), JPH::CollideShapeSettings(), hit);
    if (!hit.HadHit()) {
      continue;
    }
    const VkrPhysicsSensorEvent pair = {
        bodies.GetUserData(sensor),
        sensor_shape->GetSubShapeUserData(hit.mHit.mSubShapeID1),
        slot.desc.entity_id, 0u, false_v};
    if (!sensor_pair_add(world, pair)) {
      return false_v;
    }
  }
  return true_v;
}

static bool8_t update_sensors(VkrPhysicsWorld *world) {
  world->next_pairs.clear();
  auto &bodies = world->system.GetBodyInterface();
  for (const auto &slot : world->slots) {
    if (!slot.occupied || !slot.enabled || !slot.sensor) {
      continue;
    }
    auto shape = bodies.GetShape(slot.id);
    auto transform = bodies.GetCenterOfMassTransform(slot.id);
    s_SensorBodyFilter body_filter(slot.id);
    s_QueryFilter layer_filter(
        static_cast<uint16_t>(bodies.GetObjectLayer(slot.id) >> 16));
    s_SensorCollector collector(world, slot.id);
    world->system.GetNarrowPhaseQuery().CollideShape(
        shape, JPH::Vec3::sReplicate(1), transform, JPH::CollideShapeSettings(),
        transform.GetTranslation(), collector, {}, layer_filter, body_filter);
    if (world->faulted || !sensor_characters(world, slot.id)) {
      return false_v;
    }
  }
  for (const auto &pair : world->next_pairs) {
    bool found = false;
    for (const auto &old : world->sensor_pairs) {
      found = found || same_pair(pair, old);
    }
    if (!found && !queue_event(world, pair, true_v)) {
      return false_v;
    }
  }
  for (const auto &pair : world->sensor_pairs) {
    bool found = false;
    for (const auto &current : world->next_pairs) {
      found = found || same_pair(pair, current);
    }
    if (!found && !queue_event(world, pair, false_v)) {
      return false_v;
    }
  }
  world->sensor_pairs.swap(world->next_pairs);
  return true_v;
}

// Moves every queued event into caller storage. Null storage is accepted only
// with zero capacity, so a nonempty queue that fits always has a destination.
template <typename Event>
static bool8_t drain_events(VkrPhysicsWorld *world, std::vector<Event> *queue,
                            Event *events, uint32_t capacity,
                            uint32_t *out_count, const char *invalid_message,
                            const char *capacity_message) {
  if (world && world->dispatching) {
    return fail(world, "Physics mutation or drain during event dispatch");
  }
  if (out_count) {
    *out_count = 0;
  }
  if (!world || world->faulted || !out_count || (!events && capacity)) {
    return fail(world, invalid_message);
  }
  if (capacity < queue->size()) {
    return fail(world, capacity_message);
  }
  *out_count = static_cast<uint32_t>(queue->size());
  if (*out_count) {
    assert_log(events != NULL, "A nonempty queue implies caller storage");
    MemCopy(events, queue->data(), *out_count * sizeof(*events));
  }
  queue->clear();
  return true_v;
}

extern "C" bool8_t vkr_physics_sensor_events(VkrPhysicsWorld *world,
                                             VkrPhysicsSensorEvent *events,
                                             uint32_t capacity,
                                             uint32_t *out_count) {
  return drain_events(world, world ? &world->events : nullptr, events, capacity,
                      out_count, "Invalid sensor event output or faulted world",
                      "Sensor event output capacity exceeded");
}

extern "C" bool8_t vkr_physics_body_reserve_destroy(VkrPhysicsWorld *world,
                                                    VkrPhysicsBody body) {
  if (world && world->dispatching) {
    return fail(world, "Physics mutation or drain during event dispatch");
  }

  auto *slot = lookup(world, body);
  if (!slot || slot->destroy_reserved) {
    return fail(world, "Invalid body or duplicate prepared destruction");
  }
  const uint64_t entity_id =
      world->system.GetBodyInterface().GetUserData(slot->id);
  uint32_t exits = 0;
  if (slot->enabled) {
    for (const auto &pair : world->sensor_pairs) {
      exits += pair.entity_a == entity_id || pair.entity_b == entity_id;
    }
  }
  if (world->events.size() + world->reserved_events + exits >
      world->events.capacity()) {
    return fail(world, "Sensor event capacity cannot stage body destruction; "
                       "drain events first");
  }
  uint32_t contact_exits = 0;
  if (slot->enabled) {
    for (const auto &pair : world->contacts) {
      contact_exits += pair.event.body_a == body || pair.event.body_b == body;
    }
  }
  if (world->contact_events.size() + world->reserved_contact_events +
          contact_exits >
      world->contact_events.capacity()) {
    return fail(world, "Contact event capacity cannot stage body destruction; "
                       "drain events first");
  }
  slot->destroy_reserved = true_v;
  slot->reserved_contacts = contact_exits;
  world->reserved_contact_events += contact_exits;
  slot->reserved_events = exits;
  world->reserved_events += exits;
  ++world->reserved_destructions;
  return true_v;
}

extern "C" void vkr_physics_body_cancel_destroy(VkrPhysicsWorld *world,
                                                VkrPhysicsBody body) {
  if (world && world->dispatching) {
    fail(world, "Physics mutation during event dispatch");
    return;
  }

  auto *slot = lookup(world, body);
  if (slot && slot->destroy_reserved) {
    world->reserved_events -= slot->reserved_events;
    world->reserved_contact_events -= slot->reserved_contacts;
    --world->reserved_destructions;
    slot->destroy_reserved = false_v;
    slot->reserved_events = 0;
    slot->reserved_contacts = 0;
  }
}

extern "C" bool8_t vkr_physics_raycast_filtered(VkrPhysicsWorld *world,
                                                const float32_t origin[3],
                                                const float32_t displacement[3],
                                                uint16_t mask,
                                                VkrPhysicsRayHit *hit) {
  VkrPhysicsQueryFilter filter = {mask, true_v, nullptr, 0};
  return vkr_physics_raycast_query(world, origin, displacement, &filter, hit);
}

static bool8_t physics_cast_shape(VkrPhysicsWorld *world,
                                  const JPH::Shape *shape,
                                  JPH::RMat44Arg transform,
                                  const float32_t displacement[3],
                                  const VkrPhysicsQueryFilter *filter,
                                  VkrPhysicsRayHit *hit, bool8_t *found) {
  *hit = {};
  *found = false_v;
  auto cast = JPH::RShapeCast::sFromWorldTransform(
      shape, JPH::Vec3::sReplicate(1), transform, vec(displacement));
  JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
  JPH::ShapeCastSettings settings;
  settings.mReturnDeepestPoint = true;
  s_QueryFilter layers(filter ? filter->mask : UINT16_MAX);
  s_QueryBodyFilter bodies(filter);
  const auto base = transform.GetTranslation();
  world->system.GetNarrowPhaseQuery().CastShape(cast, settings, base, collector,
                                                {}, layers, bodies);
  if (!collector.HadHit()) {
    return true_v;
  }
  const auto &result = collector.mHit;
  auto &interface = world->system.GetBodyInterface();
  hit->body = body_handle(world, result.mBodyID2);
  hit->entity_id = interface.GetUserData(result.mBodyID2);
  hit->collider_entity_id = interface.GetShape(result.mBodyID2)
                                ->GetSubShapeUserData(result.mSubShapeID2);
  hit->fraction = result.mFraction;
  store_vec(base + result.mContactPointOn2, hit->position);
  store_vec(-result.mPenetrationAxis.NormalizedOr(JPH::Vec3::sAxisY()),
            hit->normal);
  *found = true_v;
  return true_v;
}

extern "C" bool8_t
vkr_physics_sweep(VkrPhysicsWorld *world, const VkrPhysicsColliderDesc *desc,
                  const float32_t origin[3], const float32_t rotation[4],
                  const float32_t displacement[3],
                  const VkrPhysicsQueryFilter *filter, VkrPhysicsRayHit *hit) {
  if (!world || world->faulted || !desc || !hit ||
      desc->shape == VKR_PHYSICS_TRIANGLE_MESH || !finite_vector(origin, 3) ||
      !valid_rotation(rotation) || !finite_vector(desc->position, 3) ||
      !valid_rotation(desc->rotation) || !finite_vector(displacement, 3) ||
      !valid_query_filter(filter)) {
    return fail(world, "Invalid shape sweep");
  }
  *hit = {};
  try {
    JPH::Ref<JPH::Shape> shape;
    if (!create_shape(world, *desc, shape)) {
      return false_v;
    }
    auto transform =
        JPH::RMat44::sRotationTranslation(quat(rotation), vec(origin)) *
        JPH::Mat44::sRotationTranslation(quat(desc->rotation),
                                         vec(desc->position));
    bool8_t found = false_v;
    return physics_cast_shape(world, shape, transform, displacement, filter,
                              hit, &found) &&
           found;
  } catch (...) {
    return fail(world, "Shape sweep allocation failed");
  }
}

extern "C" bool8_t
vkr_physics_sweep_sphere(VkrPhysicsWorld *world, const float32_t origin[3],
                         const float32_t displacement[3], float32_t radius,
                         const VkrPhysicsQueryFilter *filter,
                         VkrPhysicsRayHit *hit, bool8_t *found) {
  if (!world || world->faulted || !hit || !found || !finite_vector(origin, 3) ||
      !finite_vector(displacement, 3) || !std::isfinite(radius) ||
      radius <= 0 || radius > VKR_PHYSICS_MAX_COORDINATE ||
      !valid_query_filter(filter)) {
    return fail(world, "Invalid sphere sweep");
  }
  try {
    JPH::SphereShape sphere(radius);
    sphere.SetEmbedded();
    const auto transform = JPH::RMat44::sTranslation(vec(origin));
    return physics_cast_shape(world, &sphere, transform, displacement, filter,
                              hit, found);
  } catch (...) {
    return fail(world, "Sphere sweep failed");
  }
}

extern "C" bool8_t vkr_physics_body_set_velocity(VkrPhysicsWorld *world,
                                                 VkrPhysicsBody body,
                                                 const float32_t linear[3],
                                                 const float32_t angular[3]) {
  if (world && world->dispatching) {
    return fail(world, "Physics mutation or drain during event dispatch");
  }

  auto *slot = lookup(world, body);
  if (!slot || slot->destroy_reserved || slot->motion == VKR_PHYSICS_STATIC ||
      !finite_vector(linear, 3) || !finite_vector(angular, 3)) {
    return fail(world, "Invalid body velocity");
  }
  if (slot->enabled) {
    world->system.GetBodyInterface().SetLinearAndAngularVelocity(
        slot->id, vec(linear), vec(angular));
  } else {
    slot->disabled_linear = vec(linear);
    slot->disabled_angular = vec(angular);
  }
  return true_v;
}

static s_JointSlot *lookup_joint(VkrPhysicsWorld *world,
                                 VkrPhysicsJoint handle) {
  if (!world || world->faulted || !handle) {
    fail(world, "Invalid physics world or joint");
    return nullptr;
  }
  uint32_t index = static_cast<uint32_t>(handle) - 1;
  if (index >= world->joints.size() || !world->joints[index].constraint ||
      world->joints[index].generation != static_cast<uint32_t>(handle >> 32)) {
    fail(world, "Stale physics joint handle");
    return nullptr;
  }
  return &world->joints[index];
}

static void refresh_joint(VkrPhysicsWorld *world, s_JointSlot &joint) {
  auto *a = lookup(world, joint.body_a);
  auto *b = lookup(world, joint.body_b);
  joint.constraint->SetEnabled(joint.enabled && a && b && a->enabled &&
                               b->enabled);
}

static void refresh_attached_joints(VkrPhysicsWorld *world,
                                    VkrPhysicsBody body) {
  for (auto &joint : world->joints) {
    if (joint.constraint && (joint.body_a == body || joint.body_b == body)) {
      refresh_joint(world, joint);
    }
  }
}

static void remove_attached_joints(VkrPhysicsWorld *world,
                                   VkrPhysicsBody body) {
  for (auto &joint : world->joints) {
    if (joint.constraint && (joint.body_a == body || joint.body_b == body)) {
      world->system.RemoveConstraint(joint.constraint);
      joint.constraint = nullptr;
      joint.generation = 0;
      --world->joint_count;
    }
  }
}

static bool valid_joint_axes(const float32_t axis[3],
                             const float32_t normal[3]) {
  return finite_vector(axis, 3) && finite_vector(normal, 3) &&
         std::abs(vec(axis).LengthSq() - 1) < 0.0001f &&
         std::abs(vec(normal).LengthSq() - 1) < 0.0001f &&
         std::abs(vec(axis).Dot(vec(normal))) < 0.0001f;
}

static void joint_body_changed(VkrPhysicsWorld *world, VkrPhysicsBody handle) {
  auto *body = lookup(world, handle);
  if (body) {
    auto &interface = world->system.GetBodyInterface();
    interface.InvalidateContactCache(body->id);
    if (body->enabled && body->motion == VKR_PHYSICS_DYNAMIC) {
      interface.ActivateBody(body->id);
    }
  }
}

extern "C" bool8_t vkr_physics_joint_create(VkrPhysicsWorld *world,
                                            const VkrPhysicsJointDesc *desc,
                                            VkrPhysicsJoint *out_joint) {
  if (world && world->dispatching) {
    return fail(world, "Physics mutation or drain during event dispatch");
  }

  if (out_joint) {
    *out_joint = 0;
  }
  if (!desc || !out_joint || desc->body_a == desc->body_b ||
      desc->type < VKR_PHYSICS_JOINT_FIXED ||
      desc->type > VKR_PHYSICS_JOINT_SWING_TWIST ||
      !finite_vector(desc->anchor_a, 3) || !finite_vector(desc->anchor_b, 3) ||
      (desc->type != VKR_PHYSICS_JOINT_DISTANCE &&
       (!valid_joint_axes(desc->axis_a, desc->normal_a) ||
        !valid_joint_axes(desc->axis_b, desc->normal_b)))) {
    return fail(world, "Invalid joint description");
  }
  auto *a = lookup(world, desc->body_a);
  auto *b = lookup(world, desc->body_b);
  if (!a || !b || a->destroy_reserved || b->destroy_reserved ||
      (a->motion != VKR_PHYSICS_DYNAMIC && b->motion != VKR_PHYSICS_DYNAMIC)) {
    return fail(world, "Joint needs live bodies and at least one dynamic body");
  }
  if (desc->type != VKR_PHYSICS_JOINT_FIXED &&
      (!std::isfinite(desc->min_limit) || !std::isfinite(desc->max_limit) ||
       desc->min_limit > desc->max_limit ||
       (desc->type == VKR_PHYSICS_JOINT_DISTANCE
            ? (desc->min_limit < 0 ||
               desc->max_limit > VKR_PHYSICS_MAX_COORDINATE)
            : (desc->min_limit < -JPH::JPH_PI || desc->min_limit > 0 ||
               desc->max_limit < 0 || desc->max_limit > JPH::JPH_PI)))) {
    return fail(world, "Invalid joint limits");
  }
  if (desc->type == VKR_PHYSICS_JOINT_SWING_TWIST &&
      (!std::isfinite(desc->swing_normal_limit) ||
       !std::isfinite(desc->swing_plane_limit) ||
       desc->swing_normal_limit < 0 || desc->swing_normal_limit > JPH::JPH_PI ||
       desc->swing_plane_limit < 0 || desc->swing_plane_limit > JPH::JPH_PI)) {
    return fail(world, "Invalid swing cone limits");
  }
  uint32_t index = 0;
  while (index < world->joints.size() && world->joints[index].constraint) {
    ++index;
  }
  if (index == world->joints.size() || next_joint_generation > UINT32_MAX) {
    return fail(world, "Physics joint capacity exhausted");
  }
  try {
    // Synchronous ownership makes the two no-lock body accesses safe here.
    JPH::BodyLockWrite lock_a(world->system.GetBodyLockInterfaceNoLock(),
                              a->id);
    JPH::BodyLockWrite lock_b(world->system.GetBodyLockInterfaceNoLock(),
                              b->id);
    auto &body_a = lock_a.GetBody();
    auto &body_b = lock_b.GetBody();
    const auto world_a = body_a.GetWorldTransform();
    const auto world_b = body_b.GetWorldTransform();
    const auto point_a = world_a * vec(desc->anchor_a);
    const auto point_b = world_b * vec(desc->anchor_b);
    const auto axis_a = world_a.Multiply3x3(vec(desc->axis_a));
    const auto axis_b = world_b.Multiply3x3(vec(desc->axis_b));
    const auto normal_a = world_a.Multiply3x3(vec(desc->normal_a));
    const auto normal_b = world_b.Multiply3x3(vec(desc->normal_b));
    JPH::Ref<JPH::TwoBodyConstraint> constraint;
    switch (desc->type) {
    case VKR_PHYSICS_JOINT_FIXED: {
      JPH::FixedConstraintSettings settings;
      settings.mPoint1 = point_a;
      settings.mPoint2 = point_b;
      settings.mAxisX1 = axis_a;
      settings.mAxisX2 = axis_b;
      settings.mAxisY1 = normal_a;
      settings.mAxisY2 = normal_b;
      constraint = settings.Create(body_a, body_b);
      break;
    }
    case VKR_PHYSICS_JOINT_HINGE: {
      JPH::HingeConstraintSettings settings;
      settings.mPoint1 = point_a;
      settings.mPoint2 = point_b;
      settings.mHingeAxis1 = axis_a;
      settings.mHingeAxis2 = axis_b;
      settings.mNormalAxis1 = normal_a;
      settings.mNormalAxis2 = normal_b;
      settings.mLimitsMin = desc->min_limit;
      settings.mLimitsMax = desc->max_limit;
      constraint = settings.Create(body_a, body_b);
      break;
    }
    case VKR_PHYSICS_JOINT_DISTANCE: {
      JPH::DistanceConstraintSettings settings;
      settings.mPoint1 = point_a;
      settings.mPoint2 = point_b;
      settings.mMinDistance = desc->min_limit;
      settings.mMaxDistance = desc->max_limit;
      constraint = settings.Create(body_a, body_b);
      break;
    }
    case VKR_PHYSICS_JOINT_SWING_TWIST: {
      JPH::SwingTwistConstraintSettings settings;
      settings.mPosition1 = point_a;
      settings.mPosition2 = point_b;
      settings.mTwistAxis1 = axis_a;
      settings.mTwistAxis2 = axis_b;
      settings.mPlaneAxis1 = normal_a;
      settings.mPlaneAxis2 = normal_b;
      settings.mTwistMinAngle = desc->min_limit;
      settings.mTwistMaxAngle = desc->max_limit;
      settings.mNormalHalfConeAngle = desc->swing_normal_limit;
      settings.mPlaneHalfConeAngle = desc->swing_plane_limit;
      constraint = settings.Create(body_a, body_b);
      break;
    }
    }
    if (!constraint) {
      return fail(world, "Joint allocation failed");
    }
    constraint->SetEnabled(desc->enabled && a->enabled && b->enabled);
    world->system.AddConstraint(constraint);
    auto &slot = world->joints[index];
    slot.constraint = constraint;
    ++world->joint_count;
    slot.body_a = desc->body_a;
    slot.body_b = desc->body_b;
    slot.enabled = desc->enabled;
    slot.collide_connected = desc->collide_connected;
    slot.generation = static_cast<uint32_t>(next_joint_generation++);
    *out_joint = (static_cast<uint64_t>(slot.generation) << 32) | (index + 1u);
    if (slot.enabled) {
      joint_body_changed(world, slot.body_a);
      joint_body_changed(world, slot.body_b);
    }
    return true_v;
  } catch (...) {
    return fail(world, "Joint allocation failed");
  }
}

extern "C" bool8_t vkr_physics_joint_destroy(VkrPhysicsWorld *world,
                                             VkrPhysicsJoint joint) {
  if (world && world->dispatching) {
    return fail(world, "Physics mutation or drain during event dispatch");
  }

  auto *slot = lookup_joint(world, joint);
  if (!slot) {
    return false_v;
  }
  world->system.RemoveConstraint(slot->constraint);
  if (slot->enabled) {
    joint_body_changed(world, slot->body_a);
    joint_body_changed(world, slot->body_b);
  }
  slot->constraint = nullptr;
  slot->generation = 0;
  --world->joint_count;
  return true_v;
}

extern "C" bool8_t vkr_physics_joint_set_enabled(VkrPhysicsWorld *world,
                                                 VkrPhysicsJoint joint,
                                                 bool8_t enabled) {
  if (world && world->dispatching) {
    return fail(world, "Physics mutation or drain during event dispatch");
  }

  auto *slot = lookup_joint(world, joint);
  if (!slot) {
    return false_v;
  }
  slot->enabled = enabled != 0;
  refresh_joint(world, *slot);
  joint_body_changed(world, slot->body_a);
  joint_body_changed(world, slot->body_b);
  return true_v;
}

static bool same_contact(const s_ContactPair &a, const s_ContactPair &b) {
  return a.event.body_a == b.event.body_a && a.event.body_b == b.event.body_b &&
         a.event.collider_a == b.event.collider_a &&
         a.event.collider_b == b.event.collider_b;
}

static uint64_t contact_hash(const s_ContactPair &pair) {
  const uint64_t keys[4] = {pair.event.body_a, pair.event.body_b,
                            pair.event.collider_a, pair.event.collider_b};
  uint64_t hash = 0x9e3779b97f4a7c15ull;
  for (const uint64_t key : keys) {
    hash ^= key + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
  }
  hash ^= hash >> 33;
  hash *= 0xff51afd7ed558ccdull;
  return hash ^ (hash >> 33);
}

/* One pair reported by several manifolds keeps the least point and normal,
 * whatever order the manifolds arrived in. */
static bool contact_detail_before(const s_ContactPair &a,
                                  const s_ContactPair &b) {
  for (uint32_t i = 0; i < 3; ++i) {
    if (a.event.position[i] != b.event.position[i]) {
      return a.event.position[i] < b.event.position[i];
    }
  }
  for (uint32_t i = 0; i < 3; ++i) {
    if (a.event.normal[i] != b.event.normal[i]) {
      return a.event.normal[i] < b.event.normal[i];
    }
  }
  return false;
}

static void capture_contact(VkrPhysicsWorld *world, const JPH::Body &a,
                            const JPH::Body &b,
                            const JPH::ContactManifold &manifold,
                            const JPH::ContactSettings &settings) {
  if (settings.mIsSensor || world->faulted) {
    return;
  }
  s_ContactPair pair = {};
  pair.id_a = a.GetID();
  pair.id_b = b.GetID();
  pair.event.body_a = body_handle(world, pair.id_a);
  pair.event.body_b = body_handle(world, pair.id_b);
  pair.event.entity_a = a.GetUserData();
  pair.event.entity_b = b.GetUserData();
  pair.event.collider_a =
      a.GetShape()->GetSubShapeUserData(manifold.mSubShapeID1);
  pair.event.collider_b =
      b.GetShape()->GetSubShapeUserData(manifold.mSubShapeID2);
  store_vec(manifold.GetWorldSpaceContactPointOn1(0), pair.event.position);
  store_vec(manifold.mWorldSpaceNormal, pair.event.normal);
  std::lock_guard<std::mutex> lock(world->contact_mutex);
  if (world->faulted) {
    return;
  }
  // The table holds twice the contact capacity, so a probe ends at a free
  // slot before it is full.
  const size_t mask = world->contact_slots.size() - 1;
  for (size_t slot = contact_hash(pair) & mask;; slot = (slot + 1) & mask) {
    if (world->contact_stamps[slot] != world->contact_stamp) {
      if (world->next_contacts.size() == world->next_contacts.capacity()) {
        event_fault(world);
        return;
      }
      world->contact_stamps[slot] = world->contact_stamp;
      world->contact_slots[slot] =
          static_cast<uint32_t>(world->next_contacts.size());
      world->next_contacts.push_back(pair);
      return;
    }
    s_ContactPair &existing = world->next_contacts[world->contact_slots[slot]];
    if (same_contact(existing, pair)) {
      if (contact_detail_before(pair, existing)) {
        existing = pair;
      }
      return;
    }
  }
}

void s_ContactListener::OnContactAdded(const JPH::Body &a, const JPH::Body &b,
                                       const JPH::ContactManifold &manifold,
                                       JPH::ContactSettings &settings) {
  capture_contact(world, a, b, manifold, settings);
}

void s_ContactListener::OnContactPersisted(const JPH::Body &a,
                                           const JPH::Body &b,
                                           const JPH::ContactManifold &manifold,
                                           JPH::ContactSettings &settings) {
  capture_contact(world, a, b, manifold, settings);
}

static bool8_t queue_contact(VkrPhysicsWorld *world,
                             VkrPhysicsContactEvent event,
                             VkrPhysicsContactPhase phase) {
  if (world->contact_events.size() + world->reserved_contact_events >=
      world->contact_events.capacity()) {
    return event_fault(world);
  }
  event.phase = phase;
  world->contact_events.push_back(event);
  return true_v;
}

static bool8_t update_contacts(VkrPhysicsWorld *world) {
  if (world->faulted) {
    world->events.clear();
    world->contact_events.clear();
    return false_v;
  }
  // Both lists are sorted by contact key, so each pass is one merge.
  auto &bodies = world->system.GetBodyInterface();
  auto &next = world->next_contacts;
  const auto &contacts = world->contacts;
  // Resting bodies that both sleep report no contact, yet still touch.
  const size_t reported = next.size();
  size_t cursor = 0;
  for (const auto &old : contacts) {
    while (cursor < reported && contact_before(next[cursor], old)) {
      ++cursor;
    }
    const bool seen = cursor < reported && same_contact(next[cursor], old);
    if (!seen && !bodies.IsActive(old.id_a) && !bodies.IsActive(old.id_b)) {
      if (next.size() == next.capacity()) {
        return event_fault(world);
      }
      next.push_back(old);
    }
  }
  if (next.size() != reported) {
    std::inplace_merge(next.begin(), next.begin() + reported, next.end(),
                       contact_before);
  }
  cursor = 0;
  for (const auto &pair : next) {
    while (cursor < contacts.size() && contact_before(contacts[cursor], pair)) {
      ++cursor;
    }
    const bool existed =
        cursor < contacts.size() && same_contact(contacts[cursor], pair);
    if (!queue_contact(world, pair.event,
                       existed ? VKR_PHYSICS_CONTACT_PERSIST
                               : VKR_PHYSICS_CONTACT_BEGIN)) {
      return false_v;
    }
  }
  cursor = 0;
  for (const auto &old : contacts) {
    while (cursor < next.size() && contact_before(next[cursor], old)) {
      ++cursor;
    }
    const bool seen = cursor < next.size() && same_contact(next[cursor], old);
    if (!seen && !queue_contact(world, old.event, VKR_PHYSICS_CONTACT_END)) {
      return false_v;
    }
  }
  world->contacts.swap(world->next_contacts);
  return true_v;
}

/* Ends the body's contacts; the rest keep their sorted order. */
static bool8_t end_contact_pairs(VkrPhysicsWorld *world, VkrPhysicsBody body) {
  auto &contacts = world->contacts;
  size_t kept = 0;
  bool8_t ok = true_v;
  for (size_t i = 0; i < contacts.size(); ++i) {
    const s_ContactPair pair = contacts[i];
    const bool ends =
        ok && (pair.event.body_a == body || pair.event.body_b == body);
    if (ends && queue_contact(world, pair.event, VKR_PHYSICS_CONTACT_END)) {
      continue;
    }
    ok = ok && !ends;
    contacts[kept++] = pair;
  }
  contacts.resize(kept);
  return ok;
}

extern "C" bool8_t vkr_physics_contact_events(VkrPhysicsWorld *world,
                                              VkrPhysicsContactEvent *events,
                                              uint32_t capacity,
                                              uint32_t *out_count) {
  return drain_events(world, world ? &world->contact_events : nullptr, events,
                      capacity, out_count,
                      "Invalid contact event output or faulted world",
                      "Contact event output capacity exceeded");
}

JPH::ValidateResult
s_ContactListener::OnContactValidate(const JPH::Body &a, const JPH::Body &b,
                                     JPH::RVec3Arg base,
                                     const JPH::CollideShapeResult &result) {
  (void)base;
  (void)result;
  if (world->joint_count == 0) {
    return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
  }
  const auto handle_a = body_handle(world, a.GetID());
  const auto handle_b = body_handle(world, b.GetID());
  for (const auto &joint : world->joints) {
    if (joint.constraint && joint.enabled && !joint.collide_connected &&
        ((joint.body_a == handle_a && joint.body_b == handle_b) ||
         (joint.body_a == handle_b && joint.body_b == handle_a))) {
      return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
    }
  }
  return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
}

extern "C" bool8_t vkr_physics_contact_events_dispatch(
    VkrPhysicsWorld *world, VkrPhysicsContactCallback callback, void *user) {
  if (!world || world->faulted || world->dispatching) {
    return fail(world, "Invalid world or reentrant contact event dispatch");
  }
  world->dispatching = true_v;
  try {
    if (callback && !world->contact_events.empty()) {
      callback(world->contact_events.data(),
               static_cast<uint32_t>(world->contact_events.size()), user);
    }
  } catch (...) {
    world->dispatching = false_v;
    return fail(world, "Contact callback failed");
  }
  world->dispatching = false_v;
  world->contact_events.clear();
  return true_v;
}
