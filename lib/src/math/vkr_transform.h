/**
 * @file vkr_transform.h
 * @brief Transformation utilities for 3D objects
 *
 * This file provides transformation functions for graphics programming, game
 * development, and general numerical computation.
 */

#pragma once

#include "mat.h"
#include "vec.h"
#include "vkr_quat.h"

#define VKR_TRANSFORM_MAX_DEPTH 64

/**
 * @brief Transformation struct
 * @note This struct is used to store the transformation of an object
 * @note The position is the position of the object
 * @note The rotation is the rotation of the object
 * @note The scale is the scale of the object
 * @note The is_dirty flag is used to indicate if the transformation is dirty
 * @note The local is the local transformation matrix
 * @note The parent is the parent of the object
 **/
typedef struct VkrTransform {
  Vec3 position;               /**< The position of the object */
  VkrQuat rotation;            /**< The rotation of the object */
  Vec3 scale;                  /**< The scale of the object */
  bool8_t is_dirty;            /**< The dirty flag */
  Mat4 local;                  /**< The local transformation matrix */
  struct VkrTransform *parent; /**< The parent of the object */
} VkrTransform;

/**
 * @brief Creates a new transformation
 * @param position The position of the object
 * @param rotation The rotation of the object (quaternion)
 * @param scale The scale of the object
 * @return The new transformation
 **/
VkrTransform vkr_transform_new(Vec3 position, VkrQuat rotation, Vec3 scale);

/**
 * @brief Creates a new transformation from a position
 * @param position The position of the object
 * @return The new transformation
 **/
VkrTransform vkr_transform_from_position(Vec3 position);

/**
 * @brief Creates a new transformation from a rotation
 * @param rotation The rotation of the object
 * @return The new transformation
 **/
VkrTransform vkr_transform_from_rotation(VkrQuat rotation);

/**
 * @brief Creates a new transformation from a scale
 * @param scale The scale of the object
 * @return The new transformation
 **/
VkrTransform vkr_transform_from_scale(Vec3 scale);
/**
 * @brief Creates a new transformation from a position and rotation
 * @param position The position of the object
 * @param rotation The rotation of the object
 * @return The new transformation
 **/
VkrTransform vkr_transform_from_position_rotation(Vec3 position,
                                                  VkrQuat rotation);
/**
 * @brief Creates a new transformation from a position and scale and rotation
 * @param position The position of the object
 * @param scale The scale of the object
 * @param rotation The rotation of the object
 * @return The new transformation
 **/
VkrTransform vkr_transform_from_position_scale_rotation(Vec3 position,
                                                        Vec3 scale,
                                                        VkrQuat rotation);
/**
 * @brief Creates a new identity transformation
 * @return The new transformation
 **/
VkrTransform vkr_transform_identity();

/**
 * @brief Translates the transformation
 * @param transform The transformation
 * @param translation The translation
 **/
void vkr_transform_translate(VkrTransform *transform, Vec3 translation);

/**
 * @brief Rotates the transformation
 * @param transform The transformation
 * @param rotation Delta rotation applied to the current orientation
 *(quaternion)
 **/
void vkr_transform_rotate(VkrTransform *transform, VkrQuat rotation);

/**
 * @brief Scales the transformation
 * @param transform The transformation
 * @param scale The scale
 **/
void vkr_transform_scale(VkrTransform *transform, Vec3 scale);

/**
 * @brief Translates and rotates the transformation
 * @param transform The transformation
 * @param translation The translation
 * @param rotation Delta rotation applied after the translation (quaternion)
 **/
void vkr_transform_translate_rotate(VkrTransform *transform, Vec3 translation,
                                    VkrQuat rotation);

/**
 * @brief Sets the position of the transformation
 * @param transform The transformation
 * @param position The position
 **/
void vkr_transform_set_position(VkrTransform *transform, Vec3 position);

/**
 * @brief Sets the rotation of the transformation
 * @param transform The transformation
 * @param rotation The rotation
 **/
void vkr_transform_set_rotation(VkrTransform *transform, VkrQuat rotation);

/**
 * @brief Sets the scale of the transformation
 * @param transform The transformation
 * @param scale The scale
 **/
void vkr_transform_set_scale(VkrTransform *transform, Vec3 scale);

/**
 * @brief Sets the position and rotation of the transformation
 * @param transform The transformation
 * @param position The position
 * @param rotation The rotation
 **/
void vkr_transform_set_position_rotation(VkrTransform *transform, Vec3 position,
                                         VkrQuat rotation);

/**
 * @brief Sets the position, rotation and scale of the transformation
 * @param transform The transformation
 * @param position The position
 * @param rotation The rotation
 * @param scale The scale
 **/
void vkr_transform_set_transform(VkrTransform *transform, Vec3 position,
                                 VkrQuat rotation, Vec3 scale);

/**
 * @brief Sets the parent of the transformation
 * @param transform The transformation
 * @param parent The parent
 **/
void vkr_transform_set_parent(VkrTransform *transform, VkrTransform *parent);

/**
 * @brief Gets the world transformation
 * @param transform The transformation
 * @return The world transformation
 **/
Mat4 vkr_transform_get_world(VkrTransform *transform);

/**
 * @brief Gets the local transformation
 * @param transform The transformation
 * @return The local transformation
 **/
Mat4 vkr_transform_get_local(VkrTransform *transform);
