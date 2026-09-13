// Jolt's umbrella must precede its headers and VKR's Square macro.
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Geometry/ConvexHullBuilder.h>
#include "assets/vkr_collision_hull.h"
// clang-format on
#include <cmath>
#include <new>
#include <vector>

extern "C" bool8_t
vkr_collision_build_hull(const float32_t *positions, uint32_t count,
                         float32_t *out_positions, uint32_t *out_indices,
                         uint32_t *out_vertex_count, uint32_t *out_index_count,
                         const char **error) {
  if (out_vertex_count) {
    *out_vertex_count = 0;
  }
  if (out_index_count) {
    *out_index_count = 0;
  }
  if (error) {
    *error = nullptr;
  }
  if (!positions || count < 4 || count > 1048576 || !out_positions ||
      !out_indices || !out_vertex_count || !out_index_count) {
    if (error) {
      *error = "Invalid hull input or output storage";
    }
    return false_v;
  }
  for (uint32_t i = 0; i < count * 3; ++i) {
    if (!std::isfinite(positions[i]) || std::fabs(positions[i]) > 1.0e7f) {
      if (error) {
        *error = "Invalid hull coordinate";
      }
      return false_v;
    }
  }
  try {
    JPH::RegisterDefaultAllocator();
    JPH::ConvexHullBuilder::Positions points;
    points.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      points.emplace_back(positions[i * 3], positions[i * 3 + 1],
                          positions[i * 3 + 2]);
    }
    JPH::ConvexHullBuilder hull(points);
    const char *message = nullptr;
    if (hull.Initialize(256, 1.0e-5f, message) !=
        JPH::ConvexHullBuilder::EResult::Success) {
      if (error) {
        *error = "Convex hull is degenerate or requires more than256 vertices; "
                 "split the source";
      }
      return false_v;
    }
    JPH::Vec3 center;
    float volume;
    hull.GetCenterOfMassAndVolume(center, volume);
    if (!(volume > 1.0e-9f)) {
      if (error) {
        *error = "Convex hull has no positive volume";
      }
      return false_v;
    }
    std::vector<uint32_t> map(count, UINT32_MAX);
    uint32_t vertices = 0, indices = 0;
    for (const auto *face : hull.GetFaces()) {
      const auto *edge = face->mFirstEdge;
      do {
        const uint32_t source = static_cast<uint32_t>(edge->mStartIdx);
        if (map[source] == UINT32_MAX) {
          if (vertices == 256) {
            return false_v;
          }
          map[source] = vertices;
          for (uint32_t j = 0; j < 3; ++j) {
            out_positions[vertices * 3 + j] = positions[source * 3 + j];
          }
          ++vertices;
        }
        edge = edge->mNextEdge;
      } while (edge != face->mFirstEdge);
    }
    for (const auto *face : hull.GetFaces()) {
      const auto *first = face->mFirstEdge;
      auto *edge = first->mNextEdge;
      while (edge->mNextEdge != first) {
        if (indices + 3 > 1536) {
          return false_v;
        }
        out_indices[indices++] = map[first->mStartIdx];
        out_indices[indices++] = map[edge->mStartIdx];
        out_indices[indices++] = map[edge->mNextEdge->mStartIdx];
        edge = edge->mNextEdge;
      }
    }
    *out_vertex_count = vertices;
    *out_index_count = indices;
    return true_v;
  } catch (const std::bad_alloc &) {
    if (error) {
      *error = "Convex hull allocation failed";
    }
    return false_v;
  } catch (...) {
    if (error) {
      *error = "Convex hull construction failed";
    }
    return false_v;
  }
}
