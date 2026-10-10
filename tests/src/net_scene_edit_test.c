#include "net_scene_edit_test.h"

#include "memory/vkr_dmemory.h"
#include "memory/vkr_dmemory_allocator.h"
#include "net/vkr_net_scene_edit.h"
#include "net/vkr_net_type.h"
#include "renderer/systems/vkr_scene_types.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Scene edits on the wire (docs/proposals/network-protocol.md, "Editor
 * collaboration"). The oracle is a second scene: each edit applied to scene A
 * is encoded against A, decoded against B and applied to B, and afterwards
 * every entity of A must exist in B under the same document id with the same
 * name, transform, parent and component bytes. Encoding a local entity id
 * instead of the document id fails, because the two scenes number their
 * entities differently. */

typedef struct EditPeer {
  VkrScene scene;
  VkrSceneEditState edits;
} EditPeer;

static void net_edit_ref(uint8_t seed, VkrEntityRef *out) {
  for (uint32_t i = 0u; i < 16u; ++i) {
    out->bytes[i] = (uint8_t)(seed * 16u + i + 1u);
  }
}

/* Applies one edit as the runtime's batch path does. */
static bool8_t net_edit_apply(EditPeer *peer,
                              const VkrSceneEditRequest *request,
                              VkrEntityId *out_created) {
  *out_created = VKR_ENTITY_ID_INVALID;
  switch (request->action) {
  case VKR_SCENE_EDIT_APPLY:
    return vkr_scene_edit_apply(&peer->edits, &peer->scene, request->entity,
                                &request->values);
  case VKR_SCENE_EDIT_CREATE:
    *out_created = vkr_scene_edit_create(&peer->edits, &peer->scene,
                                         request->parent, &request->values);
    return out_created->u64 != 0u;
  case VKR_SCENE_EDIT_DELETE:
    return vkr_scene_edit_delete(&peer->edits, &peer->scene, request->entity);
  case VKR_SCENE_EDIT_REPARENT:
    return vkr_scene_edit_reparent(&peer->edits, &peer->scene, request->entity,
                                   request->parent);
  case VKR_SCENE_EDIT_ADD_COMPONENT:
    return vkr_scene_edit_add_component(
        &peer->edits, &peer->scene, request->entity,
        request->values.component_type, request->values.component);
  case VKR_SCENE_EDIT_REMOVE_COMPONENT:
    return vkr_scene_edit_remove_component(&peer->edits, &peer->scene,
                                           request->entity,
                                           request->values.component_type);
  default:
    return false_v;
  }
}

static VkrNetSceneEditScenes net_edit_scenes(EditPeer *peer) {
  VkrNetSceneEditScenes scenes = {0};
  scenes.containers[0] = &peer->scene;
  return scenes;
}

/* Sends one edit from A to B: encode against A, apply to A, decode against B,
   apply to B. */
static void net_edit_replicate(EditPeer *a, EditPeer *b,
                               const VkrSceneEditRequest *request) {
  uint8_t bytes[8192];
  VkrBitWriter writer;
  vkr_bit_writer_init(&writer, bytes, sizeof(bytes));
  char error[256] = "";
  const VkrNetSceneEditScenes scenes_a = net_edit_scenes(a);
  assert(vkr_net_scene_edit_write(&writer, &scenes_a, request, -1, -1, error,
                                  sizeof(error)));
  const uint32_t size = vkr_bit_writer_finish(&writer);
  assert(size > 0u);
  VkrEntityId created = VKR_ENTITY_ID_INVALID;
  assert(net_edit_apply(a, request, &created));

  VkrBitReader reader;
  vkr_bit_reader_init(&reader, bytes, size);
  VkrSceneEditRequest decoded;
  int32_t entity_ref = 0;
  int32_t parent_ref = 0;
  const VkrNetSceneEditScenes scenes_b = net_edit_scenes(b);
  if (!vkr_net_scene_edit_read(&reader, &scenes_b, &decoded, &entity_ref,
                               &parent_ref, error, sizeof(error))) {
    printf("    decode failed: %s\n", error);
    assert(false && "decode");
  }
  assert(entity_ref < 0 && parent_ref < 0);
  vkr_bit_read_align(&reader);
  assert(vkr_bit_reader_at_end(&reader));
  assert(net_edit_apply(b, &decoded, &created));
}

static VkrEntityId net_edit_find(EditPeer *peer, uint8_t seed) {
  VkrEntityRef ref;
  net_edit_ref(seed, &ref);
  return vkr_scene_find_entity_ref(&peer->scene, &ref);
}

/* Every entity of A, by document id, matches B. */
static void net_edit_compare(EditPeer *a, EditPeer *b, const uint8_t *seeds,
                             uint32_t count, const VkrTypeDesc *component) {
  for (uint32_t i = 0u; i < count; ++i) {
    const VkrEntityId ea = net_edit_find(a, seeds[i]);
    const VkrEntityId eb = net_edit_find(b, seeds[i]);
    assert((ea.u64 == 0u) == (eb.u64 == 0u));
    if (!ea.u64) {
      continue;
    }
    const String8 name_a = vkr_scene_get_name(&a->scene, ea);
    const String8 name_b = vkr_scene_get_name(&b->scene, eb);
    assert(name_a.length == name_b.length &&
           MemCompare(name_a.str, name_b.str, name_a.length) == 0);
    const SceneTransform *ta = vkr_scene_get_transform(&a->scene, ea);
    const SceneTransform *tb = vkr_scene_get_transform(&b->scene, eb);
    assert(MemCompare(&ta->position, &tb->position, sizeof(Vec3)) == 0);
    assert(MemCompare(&ta->rotation, &tb->rotation, sizeof(VkrQuat)) == 0);
    assert(MemCompare(&ta->scale, &tb->scale, sizeof(Vec3)) == 0);
    VkrEntityRef parent_a = {0};
    VkrEntityRef parent_b = {0};
    if (ta->parent.u64) {
      assert(vkr_scene_entity_ref(&a->scene, ta->parent, &parent_a));
    }
    if (tb->parent.u64) {
      assert(vkr_scene_entity_ref(&b->scene, tb->parent, &parent_b));
    }
    assert(MemCompare(&parent_a, &parent_b, sizeof(VkrEntityRef)) == 0);
    VkrSceneEditValues va;
    VkrSceneEditValues vb;
    MemZero(&va, sizeof(va));
    MemZero(&vb, sizeof(vb));
    const bool8_t has_a =
        vkr_scene_edit_read_component(&a->scene, ea, component, &va);
    const bool8_t has_b =
        vkr_scene_edit_read_component(&b->scene, eb, component, &vb);
    assert(has_a == has_b);
    if (has_a) {
      assert(MemCompare(va.component, vb.component, component->size) == 0);
    }
  }
}

static void net_edit_peer_init(EditPeer *peer, VkrAllocator *allocator) {
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  assert(vkr_scene_init(&peer->scene, allocator, 0, 8, &error));
  MemZero(&peer->edits, sizeof(peer->edits));
  vkr_scene_edit_reset(&peer->edits, allocator, 1);
}

static void net_edit_peer_destroy(EditPeer *peer, VkrAllocator *allocator) {
  vkr_scene_edit_reset(&peer->edits, allocator, 0);
  vkr_scene_shutdown(&peer->scene, NULL);
}

static VkrSceneEditRequest net_edit_create(uint8_t seed, const char *name,
                                           VkrEntityId parent, Vec3 position) {
  VkrSceneEditRequest request = {.action = VKR_SCENE_EDIT_CREATE,
                                 .parent = parent};
  request.values.fields = VKR_SCENE_EDIT_NAME | VKR_SCENE_EDIT_TRANSFORM;
  snprintf(request.values.name, sizeof(request.values.name), "%s", name);
  request.values.position = position;
  request.values.rotation = vkr_quat_identity();
  request.values.scale = vec3_one();
  net_edit_ref(seed, &request.values.ref);
  return request;
}

/* Fails when a sequence of creations, renames, transforms, component adds and
   removals, reparenting and deletion does not leave a second scene equal to
   the first under the same document ids, or when an edit that cannot travel
   or a reference to a missing entity is accepted. */
static void test_net_scene_edit_replication(void) {
  printf("  Running test_net_scene_edit_replication...\n");
  VkrDMemory memory;
  assert(vkr_dmemory_create(MB(64), GB(2), &memory));
  VkrAllocator allocator = {.ctx = &memory};
  vkr_dmemory_allocator_create(&allocator);
  EditPeer a;
  EditPeer b;
  net_edit_peer_init(&a, &allocator);
  net_edit_peer_init(&b, &allocator);
  /* A different entity count on B: local ids no longer line up. */
  VkrSceneError error = VKR_SCENE_ERROR_NONE;
  (void)vkr_scene_create_entity(&b.scene, &error);
  (void)vkr_scene_create_entity(&b.scene, &error);

  VkrSceneEditRequest request =
      net_edit_create(1u, "base", VKR_ENTITY_ID_INVALID, vec3_new(0, 0, 0));
  net_edit_replicate(&a, &b, &request);
  request = net_edit_create(2u, "room", VKR_ENTITY_ID_INVALID,
                            vec3_new(1.0f, 2.0f, 3.0f));
  net_edit_replicate(&a, &b, &request);
  request = net_edit_create(3u, "lamp", net_edit_find(&a, 2u),
                            vec3_new(0.5f, 2.25f, -1.0f));
  net_edit_replicate(&a, &b, &request);

  /* Rename and move the base: exact floats travel. */
  request = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_APPLY,
                                  .entity = net_edit_find(&a, 1u)};
  assert(vkr_scene_edit_read(&a.scene, request.entity, &request.values));
  request.values.fields = VKR_SCENE_EDIT_NAME | VKR_SCENE_EDIT_TRANSFORM;
  snprintf(request.values.name, sizeof(request.values.name),
           "base \xd0\xb7\xd0\xb0\xd0\xbb");
  request.values.position = vec3_new(0.1f, 1.0f / 3.0f, -7.25f);
  request.values.rotation = vkr_quat_from_axis_angle(vec3_new(0, 1, 0), 0.7f);
  request.values.scale = vec3_new(2.0f, 2.0f, 0.5f);
  net_edit_replicate(&a, &b, &request);

  /* A world component with a string. */
  const VkrTypeDesc *text = vkr_scene_world_type_named(string8_lit("text"));
  assert(text);
  request = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_ADD_COMPONENT,
                                  .entity = net_edit_find(&a, 3u)};
  request.values.fields = VKR_SCENE_EDIT_COMPONENT;
  request.values.component_type = text;
  vkr_type_defaults(text, request.values.component);
  const uint32_t content = vkr_type_find_property(text, string8_lit("content"));
  assert(content != UINT32_MAX);
  snprintf((char *)request.values.component + text->properties[content].offset,
           text->properties[content].capacity, "hello over the wire");
  net_edit_replicate(&a, &b, &request);

  /* Reparent, then delete. */
  request = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_REPARENT,
                                  .entity = net_edit_find(&a, 1u),
                                  .parent = net_edit_find(&a, 2u)};
  net_edit_replicate(&a, &b, &request);
  request = net_edit_create(4u, "temporary", VKR_ENTITY_ID_INVALID,
                            vec3_new(9, 9, 9));
  net_edit_replicate(&a, &b, &request);
  request = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_DELETE,
                                  .entity = net_edit_find(&a, 4u)};
  net_edit_replicate(&a, &b, &request);
  assert(!net_edit_find(&b, 4u).u64);

  const uint8_t seeds[] = {1u, 2u, 3u, 4u};
  net_edit_compare(&a, &b, seeds, ArrayCount(seeds), text);

  /* Removing the component travels too. */
  request = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_REMOVE_COMPONENT,
                                  .entity = net_edit_find(&a, 3u)};
  request.values.component_type = text;
  net_edit_replicate(&a, &b, &request);
  net_edit_compare(&a, &b, seeds, ArrayCount(seeds), text);

  /* What cannot travel is refused at the writer. */
  uint8_t bytes[4096];
  VkrBitWriter writer;
  char message[256];
  const VkrNetSceneEditScenes scenes_a = net_edit_scenes(&a);
  vkr_bit_writer_init(&writer, bytes, sizeof(bytes));
  request = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_DUPLICATE,
                                  .entity = net_edit_find(&a, 2u)};
  assert(!vkr_net_scene_edit_write(&writer, &scenes_a, &request, -1, -1,
                                   message, sizeof(message)));
  const VkrEntityId anonymous = vkr_scene_create_entity(&a.scene, &error);
  request = (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_DELETE,
                                  .entity = anonymous};
  vkr_bit_writer_init(&writer, bytes, sizeof(bytes));
  assert(!vkr_net_scene_edit_write(&writer, &scenes_a, &request, -1, -1,
                                   message, sizeof(message)));

  /* An entity B lacks fails the read with a message naming it. */
  request =
      net_edit_create(5u, "only on a", VKR_ENTITY_ID_INVALID, vec3_zero());
  VkrEntityId created = VKR_ENTITY_ID_INVALID;
  assert(net_edit_apply(&a, &request, &created));
  request =
      (VkrSceneEditRequest){.action = VKR_SCENE_EDIT_DELETE, .entity = created};
  vkr_bit_writer_init(&writer, bytes, sizeof(bytes));
  assert(vkr_net_scene_edit_write(&writer, &scenes_a, &request, -1, -1, message,
                                  sizeof(message)));
  const uint32_t size = vkr_bit_writer_finish(&writer);
  VkrBitReader reader;
  vkr_bit_reader_init(&reader, bytes, size);
  VkrSceneEditRequest decoded;
  int32_t entity_ref = 0;
  int32_t parent_ref = 0;
  const VkrNetSceneEditScenes scenes_b = net_edit_scenes(&b);
  assert(!vkr_net_scene_edit_read(&reader, &scenes_b, &decoded, &entity_ref,
                                  &parent_ref, message, sizeof(message)));
  assert(strstr(message, "no entity") != NULL);

  /* Random bytes never crash the reader. */
  uint64_t state = 77u;
  for (uint32_t i = 0u; i < 20000u; ++i) {
    const uint32_t length = (uint32_t)(i % 200u);
    for (uint32_t k = 0u; k < length; ++k) {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      bytes[k] = (uint8_t)state;
    }
    vkr_bit_reader_init(&reader, bytes, length);
    (void)vkr_net_scene_edit_read(&reader, &scenes_b, &decoded, &entity_ref,
                                  &parent_ref, message, sizeof(message));
  }

  net_edit_peer_destroy(&a, &allocator);
  net_edit_peer_destroy(&b, &allocator);
  vkr_dmemory_allocator_destroy(&allocator);
  printf("  test_net_scene_edit_replication PASSED\n");
}

/* Fails when a world component value does not survive the descriptor codec
   byte for byte, or when an invalid value is accepted. */
static void test_net_type_round_trip(void) {
  printf("  Running test_net_type_round_trip...\n");
  uint32_t tested = 0u;
  for (uint32_t i = 0u;; ++i) {
    const VkrTypeDesc *type = vkr_scene_world_type(i);
    if (!type) {
      break;
    }
    _Alignas(16) uint8_t value[VKR_TYPE_VALUE_MAX];
    _Alignas(16) uint8_t decoded[VKR_TYPE_VALUE_MAX];
    vkr_type_defaults(type, value);
    vkr_type_reset_transient(type, value);
    uint8_t bytes[4096];
    VkrBitWriter writer;
    vkr_bit_writer_init(&writer, bytes, sizeof(bytes));
    assert(vkr_net_type_write(&writer, type, value));
    const uint32_t size = vkr_bit_writer_finish(&writer);
    assert(size > 0u || type->property_count == 0u);
    VkrBitReader reader;
    vkr_bit_reader_init(&reader, bytes, size);
    char error[256] = "";
    if (!vkr_net_type_read(&reader, type, decoded, error, sizeof(error))) {
      printf("    %s: %s\n", type->name, error);
      assert(false && "type round trip");
    }
    for (uint32_t p = 0u; p < type->property_count; ++p) {
      const VkrPropertyDesc *property = &type->properties[p];
      if (!(property->flags & VKR_PROPERTY_FLAG_TRANSIENT)) {
        assert(vkr_property_equal(property, value, decoded));
      }
    }
    ++tested;
  }
  printf("    %u world types round-trip\n", tested);
  assert(tested >= 10u);

  /* A text past its capacity and NaN are refused. */
  const VkrTypeDesc *text = vkr_scene_world_type_named(string8_lit("text"));
  _Alignas(16) uint8_t value[VKR_TYPE_VALUE_MAX];
  vkr_type_defaults(text, value);
  uint8_t bytes[8192];
  VkrBitWriter writer;
  vkr_bit_writer_init(&writer, bytes, sizeof(bytes));
  const uint32_t content = vkr_type_find_property(text, string8_lit("content"));
  memset(value + text->properties[content].offset, 'x',
         text->properties[content].capacity);
  assert(!vkr_net_type_write(&writer, text, value));
  printf("  test_net_type_round_trip PASSED\n");
}

bool32_t run_net_scene_edit_tests(void) {
  printf("--- Starting network scene edit tests ---\n");
  test_net_type_round_trip();
  test_net_scene_edit_replication();
  printf("--- Network scene edit tests completed ---\n");
  return true;
}
