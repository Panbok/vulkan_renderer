#pragma once

#include "defines.h"
#include "vkr_frame_input.h"
#include "vkr_gpu_abi.h"

/* The light table a backend keeps on the GPU (clustered binning proposal,
 * item 1; ADR-019): each frame's packed light rows and the rows that differ
 * from the table the last submitted frame left, so a backend that keeps its
 * table between frames writes only those. Rows carry intensity before
 * exposure and, on the light head's rows, the first local shadow view. */

/** One changed row: its index in the table and its packed value. */
typedef struct VkrLightRowUpdate {
  uint32_t row;
  uint32_t reserved[3];
  VkrGpuPointLightRow value;
} VkrLightRowUpdate;

_Static_assert(sizeof(VkrLightRowUpdate) == 80u,
               "VkrLightRowUpdate keeps its row 16-byte aligned");

/** A frame's light table, borrowed from the renderer until the frame is
 * submitted or abandoned. */
typedef struct VkrLightTableFrame {
  /** Every row of the frame's table, packed, `count` of them. */
  const VkrGpuPointLightRow *rows;
  uint32_t count;
  /** The rows that differ from the table the last submitted frame left,
   * ascending, each once; every row when `full`. */
  const VkrLightRowUpdate *updates;
  uint32_t update_count;
  /** The backend's table holds nothing to keep: the first frame, or the
   * first after vkr_light_table_invalidate. */
  bool8_t full;
} VkrLightTableFrame;

typedef struct s_VkrLightTable VkrLightTable;

/** A table with nothing submitted, so its first frame is full; NULL when
 * the allocation fails. */
VkrLightTable *vkr_light_table_create(void);

void vkr_light_table_destroy(VkrLightTable *table);

/** Packs `lighting`'s rows, with `local_shadow`'s first views on the head's
 * rows when given, and lists the rows that differ from the last committed
 * frame. The frame stays valid until the next prepare. */
void vkr_light_table_prepare(VkrLightTable *table,
                             const VkrFrameLighting *lighting,
                             const VkrLocalShadowPassPayload *local_shadow,
                             VkrLightTableFrame *out_frame);

/** The prepared frame was submitted: its rows are what the backend's table
 * now holds. A frame never committed leaves its changes to the next one. */
void vkr_light_table_commit(VkrLightTable *table);

/** The backend's table no longer holds the committed rows (device loss,
 * backend reset): the next frame is full. */
void vkr_light_table_invalidate(VkrLightTable *table);
