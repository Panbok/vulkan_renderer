/**
 * @file vkr_type_desc.h
 * @brief Static type descriptors and property tables (ADR-076).
 *
 * A type descriptor describes one plain-data type: its properties, their
 * storage, display metadata and validation. The same table drives JSON
 * serialization, validation, the editor's Details panel and Cmd evaluator
 * paths, so a type is described once instead of once per consumer.
 *
 * Descriptors are static const data owned by the translation unit that owns
 * the described type. Properties never describe heap pointers, String8 views
 * or dynamic arrays; values are copied by size.
 */
#pragma once

#include "containers/str.h"
#include "core/vkr_entity_ref.h"
#include "core/vkr_json.h"
#include "core/vkr_json_writer.h"
#include "defines.h"
#include "math/vec.h"
#include "memory/vkr_allocator.h"

/** Parse a canonical 36-character UUID in either case from `length`
    bytes. */
bool8_t vkr_entity_ref_parse(const char *text, uint64_t length,
                             VkrEntityRef *out);
/** Lowercase canonical text with a terminator. */
void vkr_entity_ref_format(const VkrEntityRef *id, char out[37]);

/** Largest described value; readers stage candidates in bounded storage. */
#define VKR_TYPE_VALUE_MAX 1024u
/** Properties per type; a duplicate-key mask needs one bit each. */
#define VKR_TYPE_PROPERTY_MAX 64u

typedef enum VkrPropertyKind {
  VKR_PROPERTY_BOOL = 0, /**< bool8_t, 0 or 1. */
  VKR_PROPERTY_I32,
  VKR_PROPERTY_U32,
  VKR_PROPERTY_F32,
  /** float32_t radians. Range, step and display use degrees. */
  VKR_PROPERTY_ANGLE,
  VKR_PROPERTY_VEC2,
  VKR_PROPERTY_VEC3,
  VKR_PROPERTY_VEC4,
  /** Vec4 quaternion; finite and nonzero. Displayed as Euler X, Y and Z
   * rotations in degrees. */
  VKR_PROPERTY_QUAT,
  /** Vec3 linear RGB. */
  VKR_PROPERTY_COLOR,
  /** Vec3 direction, displayed as yaw and elevation in degrees. Yaw zero
   * faces -Z; elevation is toward +Y. The owning type's validate hook
   * requires a nonzero length where the direction is used. */
  VKR_PROPERTY_DIRECTION,
  /** Four-byte integer or C enum whose values index `names`. */
  VKR_PROPERTY_ENUM,
  /** char[capacity], always null-terminated in storage. */
  VKR_PROPERTY_STRING,
  /** VkrEntityRef to an entity of the owner's container. JSON holds the
   * canonical UUID text, or "" for none; the owner resolves it to an entity
   * when it publishes. */
  VKR_PROPERTY_ENTITY,
  VKR_PROPERTY_KIND_COUNT
} VkrPropertyKind;

typedef enum VkrPropertyFlags {
  VKR_PROPERTY_FLAG_NONE = 0u,
  /** Shown but not editable in Details; still serialized. */
  VKR_PROPERTY_FLAG_READ_ONLY = 1u << 0,
  /** Serialized but not shown. */
  VKR_PROPERTY_FLAG_HIDDEN = 1u << 1,
  /** Runtime state: never serialized, read, undone or edited. */
  VKR_PROPERTY_FLAG_TRANSIENT = 1u << 2,
  /** Details draws a slider across [min, max]. */
  VKR_PROPERTY_FLAG_SLIDER = 1u << 3,
  /** STRING edited in a multi-line text area whose keystrokes apply as they
   * are typed. */
  VKR_PROPERTY_FLAG_MULTILINE = 1u << 4,
  /** VEC4 holding linear RGBA, edited like a COLOR with a swatch and the
   * color picker. */
  VKR_PROPERTY_FLAG_COLOR = 1u << 5,
  /** A COLOR or color VEC4 whose RGB is display-encoded sRGB rather than
   * linear; alpha stays linear. */
  VKR_PROPERTY_FLAG_SRGB = 1u << 6,
} VkrPropertyFlags;

/** Per-frame presentation state a type reports for one property. */
typedef enum VkrPropertyStateFlags {
  VKR_PROPERTY_STATE_NONE = 0u,
  VKR_PROPERTY_STATE_DISABLED = 1u << 0,
  VKR_PROPERTY_STATE_HIDDEN = 1u << 1,
} VkrPropertyStateFlags;

typedef struct VkrPropertyState {
  uint32_t flags; /**< VkrPropertyStateFlags. */
  /** Optional label replacing the static one; borrowed for the UI build. */
  String8 label;
} VkrPropertyState;

typedef struct VkrPropertyDesc {
  /** Stable snake_case key used by JSON and Cmd paths. */
  const char *name;
  const char *label;
  const char *tooltip;
  /** Details sub-heading starting at this property; NULL continues. */
  const char *group;
  /** Display suffix such as "%", "EV" or "K"; NULL for none. */
  const char *unit;
  /** Shown instead of a zero scalar when zero has a special meaning, such as
   * "Unlimited" or "Off"; NULL shows the number. Zero is then valid beside
   * [min, max]. */
  const char *zero_label;
  /** Value names, NULL-terminated; the index is the stored value. ENUM
   * serializes the name. A U32 with names displays them as choices but
   * serializes the number, keeping numeric documents compatible. */
  const char *const *names;
  /** Display names parallel to `names`; NULL shows the names. */
  const char *const *labels;
  uint32_t offset;
  /** STRING byte capacity including the terminator. */
  uint32_t capacity;
  VkrPropertyKind kind;
  uint32_t flags; /**< VkrPropertyFlags. */
  /** Inclusive bounds per component in stored units (degrees for ANGLE).
   * Bounds apply only when min < max. */
  float32_t min;
  float32_t max;
  /** Drag step per point in display units; zero uses a kind default. */
  float32_t step;
  /** Display value is stored value times this; zero means one. */
  float32_t display_scale;
} VkrPropertyDesc;

typedef enum VkrTypeFlags {
  VKR_TYPE_FLAG_NONE = 0u,
  /** At most one instance takes effect per frame; resolution picks it. */
  VKR_TYPE_FLAG_SINGLETON = 1u << 0,
  /** Only the root World holds it; every scene resolves the World's. */
  VKR_TYPE_FLAG_WORLD_ONLY = 1u << 1,
  /** Its properties change between builds, as script components' do:
   * readers skip members it no longer has and keep the default for a member
   * whose value no longer fits its property. */
  VKR_TYPE_FLAG_TOLERANT = 1u << 2,
} VkrTypeFlags;

typedef struct VkrTypeDesc {
  /** Stable snake_case type name used by documents and Cmd paths. */
  const char *name;
  const char *label;
  /** Details and browser grouping, such as "Lighting" or "Atmosphere". */
  const char *category;
  uint32_t flags; /**< VkrTypeFlags. */
  const VkrPropertyDesc *properties;
  uint32_t property_count;
  uint32_t size;
  /** Natural alignment of the described type; ECS storage uses it. */
  uint32_t align;
  /** Nonzero: serialized objects carry and require this "version". */
  uint32_t version;
  /** Names older documents used for since-removed properties. Readers accept
   * and ignore them; NULL-terminated. Optional. */
  const char *const *retired;
  /** Fill a value with the type's defaults. Optional; zero otherwise. */
  void (*defaults)(void *value);
  /** Cross-property rules after per-property checks. Optional. */
  bool8_t (*validate)(const void *value, char *error, uint32_t capacity);
  /** Enforce dependent values after an interactive edit. Optional. */
  void (*normalize)(void *value);
  /** Presentation state with caller context, such as feature availability.
   * Optional. */
  VkrPropertyState (*state)(const void *value, uint32_t property,
                            const void *context);
} VkrTypeDesc;

// =============================================================================
// Properties
// =============================================================================

/** Number of float components for vector-like kinds; one for scalars. */
uint32_t vkr_property_components(const VkrPropertyDesc *property);

/** Display multiplier: degrees for ANGLE, else display_scale or one. */
float64_t vkr_property_display_scale(const VkrPropertyDesc *property);

/** Number of value names on an ENUM or U32 property, else zero. */
uint32_t vkr_property_enum_count(const VkrPropertyDesc *property);

/** Property index by name, or UINT32_MAX. */
uint32_t vkr_type_find_property(const VkrTypeDesc *type, String8 name);

/** Stored scalar value (BOOL, I32, U32, F32, ANGLE, ENUM). ANGLE returns
 * radians. False for other kinds. */
bool8_t vkr_property_get_number(const VkrPropertyDesc *property,
                                const void *value, float64_t *out);

/** Store a scalar, rounding integer kinds. Rejects non-finite values,
 * out-of-range integers and ENUM values without a name; does not apply the
 * property's bounds, which validation owns. */
bool8_t vkr_property_set_number(const VkrPropertyDesc *property, void *value,
                                float64_t number);

/** Copy up to four float components of a vector-like or F32/ANGLE property. */
bool8_t vkr_property_get_floats(const VkrPropertyDesc *property,
                                const void *value, float32_t out[4]);

/** Store float components; a Vec3's W lane stays zero. */
bool8_t vkr_property_set_floats(const VkrPropertyDesc *property, void *value,
                                const float32_t in[4]);

/** Yaw and elevation in degrees for a nonzero direction. */
void vkr_property_direction_angles(Vec3 direction, float32_t *yaw_degrees,
                                   float32_t *elevation_degrees);

/** Unit direction for yaw and elevation in degrees. */
Vec3 vkr_property_direction_from_angles(float32_t yaw_degrees,
                                        float32_t elevation_degrees);

/** Euler X, Y and Z rotations in degrees for a quaternion. */
void vkr_property_quat_euler(Vec4 rotation, float32_t out_degrees[3]);

/** Quaternion for Euler X, Y and Z rotations in degrees. */
Vec4 vkr_property_quat_from_euler(const float32_t degrees[3]);

// =============================================================================
// Values
// =============================================================================

/** Check every property (finite, bounds, bool, enum and string rules), then
 * the type's validate hook. Writes a message on failure when error is set. */
bool8_t vkr_type_validate(const VkrTypeDesc *type, const void *value,
                          char *error, uint32_t capacity);

/** Default value of a type: its defaults hook, or zero bytes. */
void vkr_type_defaults(const VkrTypeDesc *type, void *value);

/** Presentation state of one property; NONE without a state hook. */
VkrPropertyState vkr_type_property_state(const VkrTypeDesc *type,
                                         const void *value, uint32_t property,
                                         const void *context);

/** Converts a value of `from` into `to`, two layouts of one type: defaults
 * first, then each property of `to` that `from` has by name. The same kind
 * copies; scalar numbers convert between BOOL, I32, U32, F32, ANGLE and
 * ENUM; float vectors (VEC2 to QUAT) copy their shared components; STRING
 * copies within the new capacity. Numbers clamp to the new bounds; a value
 * that does not convert keeps the default. */
void vkr_type_migrate(const VkrTypeDesc *from, const void *from_value,
                      const VkrTypeDesc *to, void *to_value);

/** Byte-compare one property's storage. */
bool8_t vkr_property_equal(const VkrPropertyDesc *property, const void *a,
                           const void *b);

/** Write a validated value as one JSON object. Transient properties are
 * omitted; the version member leads when the type declares one. */
bool8_t vkr_type_write_json(VkrJsonWriter *writer, const VkrTypeDesc *type,
                            const void *value);

/** Read one JSON object at the reader's position into `value`. Members
 * missing from the object keep their current value. Unknown, transient or
 * duplicate names, wrong JSON types, a missing or different version and an
 * invalid result fail and leave `value` unchanged. `scratch` decodes STRING
 * properties and may be NULL for types without them. On success the reader is
 * positioned after the closing brace. */
bool8_t vkr_type_read_json(VkrJsonReader *reader, const VkrTypeDesc *type,
                           void *value, VkrAllocator *scratch, char *error,
                           uint32_t capacity);

/** vkr_type_read_json over a whole document: only whitespace may follow. */
bool8_t vkr_type_read_json_document(String8 json, const VkrTypeDesc *type,
                                    void *value, VkrAllocator *scratch,
                                    char *error, uint32_t capacity);
