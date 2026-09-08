#include "vkr_color_grading.h"

enum { VKR_COLOR_GRADING_AXIS_COUNT = 3u };

typedef struct VkrColorGradingMatrix3 {
  float64_t row[VKR_COLOR_GRADING_AXIS_COUNT][VKR_COLOR_GRADING_AXIS_COUNT];
} VkrColorGradingMatrix3;

/* Unity ColorUtils StandardIlluminantY / CIExyToLMS, Graphics repository,
   ColorUtils.cs lines 39-82. The public inputs are normalized directly, rather
   than Unity's UI-scale offsets divided by 65. */
vkr_global const VkrColorGradingMatrix3 s_rgb_to_xyz = {
    {{0.4124564, 0.3575761, 0.1804375},
     {0.2126729, 0.7151522, 0.0721750},
     {0.0193339, 0.1191920, 0.9503041}}};
vkr_global const VkrColorGradingMatrix3 s_xyz_to_rgb = {
    {{3.2404542, -1.5371385, -0.4985314},
     {-0.9692660, 1.8760108, 0.0415560},
     {0.0556434, -0.2040259, 1.0572252}}};
vkr_global const VkrColorGradingMatrix3 s_cat02 = {
    {{0.7328, 0.4296, -0.1624},
     {-0.7036, 1.6975, 0.0061},
     {0.0030, 0.0136, 0.9834}}};
vkr_global const VkrColorGradingMatrix3 s_cat02_inverse = {
    {{1.096123820835514, -0.278869000218287, 0.182745179382773},
     {0.454369041975359, 0.473533154307412, 0.072097803717229},
     {-0.009627608738429, -0.005698031216113, 1.015325639954543}}};

vkr_internal VkrColorGradingGpu vkr_color_grading_identity(void) {
  return (VkrColorGradingGpu){
      .white_balance_rows = {{1.0f, 0.0f, 0.0f, 0.0f},
                             {0.0f, 1.0f, 0.0f, 0.0f},
                             {0.0f, 0.0f, 1.0f, 0.0f}},
      .controls = {1.0f, 1.0f, 0.0f, 0.0f},
  };
}

vkr_internal VkrColorGradingMatrix3 vkr_color_grading_matrix_multiply(
    VkrColorGradingMatrix3 left, VkrColorGradingMatrix3 right) {
  VkrColorGradingMatrix3 result = {0};
  for (uint32_t row = 0u; row < VKR_COLOR_GRADING_AXIS_COUNT; ++row) {
    for (uint32_t column = 0u; column < VKR_COLOR_GRADING_AXIS_COUNT;
         ++column) {
      for (uint32_t term = 0u; term < VKR_COLOR_GRADING_AXIS_COUNT; ++term)
        result.row[row][column] +=
            left.row[row][term] * right.row[term][column];
    }
  }
  return result;
}

vkr_internal void vkr_color_grading_cat02_lms(float64_t x, float64_t y,
                                               float64_t out_lms[3]) {
  const float64_t xyz[3] = {x / y, 1.0, (1.0 - x - y) / y};
  for (uint32_t row = 0u; row < VKR_COLOR_GRADING_AXIS_COUNT; ++row) {
    out_lms[row] = s_cat02.row[row][0] * xyz[0] +
                   s_cat02.row[row][1] * xyz[1] +
                   s_cat02.row[row][2] * xyz[2];
  }
}

VkrColorGradingGpu vkr_color_grading_prepare(float32_t temperature,
                                              float32_t tint,
                                              float32_t contrast,
                                              float32_t saturation) {
  VkrColorGradingGpu result = vkr_color_grading_identity();
  if (temperature == 0.0f && tint == 0.0f && contrast == 1.0f &&
      saturation == 1.0f)
    return result;
  if (temperature == 0.0f && tint == 0.0f) {
    result.controls = (Vec4){contrast, saturation, 1.0f, 0.0f};
    return result;
  }

  const float64_t temperature64 = (float64_t)temperature;
  const float64_t x = 0.31271 -
                      temperature64 * (temperature64 < 0.0 ? 0.1 : 0.05);
  const float64_t y =
      2.87 * x - 3.0 * x * x - 0.27509507 + (float64_t)tint * 0.05;
  const float64_t destination_x = 0.31271;
  const float64_t destination_y = 2.87 * destination_x -
                                  3.0 * destination_x * destination_x -
                                  0.27509507;
  float64_t source_lms[3];
  float64_t destination_lms[3];
  vkr_color_grading_cat02_lms(x, y, source_lms);
  vkr_color_grading_cat02_lms(destination_x, destination_y, destination_lms);

  VkrColorGradingMatrix3 diagonal = {0};
  /* Frame validation proves positive CAT02 coefficients for the full input
     range. The resulting RGB rows lie in [-0.619, 3.389], so binary32
     packing remains finite and bounded. */
  for (uint32_t i = 0u; i < VKR_COLOR_GRADING_AXIS_COUNT; ++i) {
    diagonal.row[i][i] = destination_lms[i] / source_lms[i];
  }
  const VkrColorGradingMatrix3 adaptation = vkr_color_grading_matrix_multiply(
      s_cat02_inverse, vkr_color_grading_matrix_multiply(diagonal, s_cat02));
  const VkrColorGradingMatrix3 white_balance =
      vkr_color_grading_matrix_multiply(
          s_xyz_to_rgb,
          vkr_color_grading_matrix_multiply(adaptation, s_rgb_to_xyz));

  for (uint32_t row = 0u; row < VKR_COLOR_GRADING_AXIS_COUNT; ++row) {
    result.white_balance_rows[row] =
        (Vec4){(float32_t)white_balance.row[row][0],
               (float32_t)white_balance.row[row][1],
               (float32_t)white_balance.row[row][2], 0.0f};
  }
  result.controls = (Vec4){contrast, saturation, 1.0f, 0.0f};
  return result;
}
