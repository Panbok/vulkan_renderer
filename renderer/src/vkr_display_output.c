#include "vkr_display_output.h"

#include <math.h>

VkrDisplayOutputParams
vkr_display_output_resolve(VkrDisplayOutputMode mode,
                           VkrDisplayOutputSnapshot snapshot,
                           bool8_t windowed) {
  VkrDisplayOutputParams result = {.headroom = 1.0f, .output_scale = 1.0f};
  if (mode != VKR_DISPLAY_OUTPUT_AUTO_EXTENDED_LINEAR || !windowed ||
      !snapshot.available || !isfinite(snapshot.headroom) ||
      !isfinite(snapshot.output_scale) || snapshot.headroom < 1.0f ||
      snapshot.output_scale <= 0.0f ||
      (float64_t)snapshot.headroom * snapshot.output_scale > 65504.0)
    return result;
  result.headroom = snapshot.headroom;
  result.output_scale = snapshot.output_scale;
  result.extended_linear = 1u;
  return result;
}
