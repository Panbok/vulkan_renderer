/**
 * @file vkr_math.h
 * @brief Comprehensive collection of inline mathematical utilities
 *
 * This file provides a complete set of fast, inline mathematical functions
 * optimized for graphics programming, game development, and general numerical
 * computation. All functions use the `float32_t` type for consistency and
 * performance on modern hardware.
 *
 * Function Categories:
 *
 * 1. **Angle Conversion**: Convert between radians and degrees
 *    - vkr_to_radians(), vkr_to_degrees()
 *
 * 2. **Basic Math Operations**: Common mathematical operations
 *    - vkr_min_f32(), vkr_max_f32(), vkr_clamp_f32(), vkr_abs_f32(),
 * vkr_sign_f32()
 *
 * 3. **Interpolation**: Linear interpolation functions
 *    - vkr_lerp_f32()
 *
 * 4. **Power and Root Functions**: Exponential and logarithmic operations
 *    - vkr_sqrt_f32(), vkr_sqrt_f64(), vkr_pow_f32(), vkr_exp_f32(),
 * vkr_log_f32()
 *
 * 5. **Rounding Functions**: Float-to-integer rounding operations
 *    - vkr_floor_f32(), vkr_ceil_f32(), vkr_round_f32()
 *
 * 6. **Trigonometric Functions**: Complete set of trig functions
 *    - vkr_sin_f32(), vkr_cos_f32(), vkr_tan_f32()
 *    - vkr_asin_f32(), vkr_acos_f32(), vkr_atan_f32(), vkr_atan2_f32()
 *
 * 7. **Random Number Generation**: Pseudo-random number utilities
 *    - vkr_rand_f32(), vkr_rand_range_f32(), vkr_rand_i32(),
 * vkr_rand_range_i32()
 *
 * Performance Notes:
 * - All functions are marked INLINE for maximum performance
 * - Functions directly wrap optimized C math library calls
 * - Random number generator is automatically seeded on first use
 * - No dynamic memory allocation is performed
 *
 * Thread Safety:
 * - Most functions are thread-safe as they don't modify global state
 * - Random number functions use global state and are NOT thread-safe
 * - For multi-threaded applications, consider using thread-local RNG
 *
 * Usage Example:
 * ```c
 * // Angle conversion
 * float32_t angle_rad = vkr_to_radians(45.0f);  // Convert 45° to radians
 * float32_t angle_deg = vkr_to_degrees(VKR_PI); // Convert π to degrees
 *
 * // Clamping and interpolation
 * float32_t health = vkr_clamp_f32(damage, 0.0f, 100.0f);
 * float32_t smooth_pos = vkr_lerp_f32(start_pos, end_pos, 0.5f);
 *
 * // Random number generation
 * float32_t random_speed = vkr_rand_range_f32(1.0f, 10.0f);
 * int32_t random_index = vkr_rand_range_i32(0, array_size - 1);
 * ```
 */

#pragma once

#include "defines.h"
#include "platform/vkr_platform.h"

/** @brief Mathematical constant π (pi) with high precision
 *  @note Value: 3.14159265358979323846...
 *  @note Uses 17 significant digits for optimal float32_t accuracy */
#define VKR_PI 3.14159265358979323846f

/** @brief Mathematical constant 2π (two pi)
 *  @note Useful for complete circle calculations
 *  @note Computed as 2.0 * PI for compile-time evaluation */
#define VKR_PI_2 2.0f * VKR_PI

/** @brief Mathematical constant π/2 (half pi)
 *  @note Represents 90 degrees in radians
 *  @note Computed as 0.5 * PI for compile-time evaluation */
#define VKR_HALF_PI 0.5f * VKR_PI

/** @brief Mathematical constant π/4 (quarter pi)
 *  @note Represents 45 degrees in radians
 *  @note Computed as 0.25 * PI for compile-time evaluation */
#define VKR_QUARTER_PI 0.25f * VKR_PI

/** @brief Reciprocal of π (1/π)
 *  @note Useful for avoiding division by PI in calculations
 *  @note Computed as 1.0 / PI for compile-time evaluation */
#define VKR_ONE_OVER_PI 1.0f / VKR_PI

/** @brief Reciprocal of 2π (1/(2π))
 *  @note Useful for frequency calculations and normalization
 *  @note Computed as 1.0 / PI_2 for compile-time evaluation */
#define VKR_ONE_OVER_TWO_PI 1.0f / VKR_PI_2

/** @brief Square root of 2 (√2) with high precision
 *  @note Value: 1.41421356237309504880...
 *  @note Useful for diagonal calculations and normalization */
#define VKR_SQRT_TWO 1.41421356237309504880f

/** @brief Square root of 3 (√3) with high precision
 *  @note Value: 1.73205080756887729352...
 *  @note Useful for triangular and hexagonal calculations */
#define VKR_SQRT_THREE 1.73205080756887729352f

/** @brief Square root of 1/2 (1/√2) with high precision
 *  @note Value: 0.70710678118654752440...
 *  @note Equivalent to √2/2, useful for 45-degree rotations */
#define VKR_SQRT_ONE_OVER_TWO 0.70710678118654752440f

/** @brief Square root of 1/3 (1/√3) with high precision
 *  @note Value: 0.57735026918962576450...
 *  @note Equivalent to √3/3, useful for equilateral triangle calculations */
#define VKR_SQRT_ONE_OVER_THREE 0.57735026918962576450f

/** @brief Conversion multiplier from degrees to radians
 *  @note Multiply degrees by this value to get radians
 *  @note Computed as PI / 180.0 for compile-time evaluation
 *  @example 45.0f * DEG2RAD_MULTIPLIER = π/4 radians */
#define VKR_DEG2RAD_MULTIPLIER VKR_PI / 180.0f

/** @brief Conversion multiplier from radians to degrees
 *  @note Multiply radians by this value to get degrees
 *  @note Computed as 180.0 / PI for compile-time evaluation
 *  @example PI * RAD2DEG_MULTIPLIER = 180.0f degrees */
#define VKR_RAD2DEG_MULTIPLIER 180.0f / VKR_PI

/** @brief Conversion multiplier from seconds to milliseconds
 *  @note Multiply seconds by this value to get milliseconds
 *  @note Value: 1000.0f (1 second = 1000 milliseconds) */
#define VKR_SEC_TO_MS_MULTIPLIER 1000.0f

/** @brief Conversion multiplier from milliseconds to seconds
 *  @note Multiply milliseconds by this value to get seconds
 *  @note Value: 0.001f (1 millisecond = 0.001 seconds) */
#define VKR_MS_TO_SEC_MULTIPLIER 0.001f

/** @brief Large value representing positive infinity for float32_t
 *  @note Value: 1e30f (1 × 10^30)
 *  @note Use for bounds checking and initialization of minimum values
 *  @note Should be larger than any realistic value in your application */
#ifndef INFINITY
#define INFINITY 1e30f
#endif

/** @brief Machine epsilon for float32_t precision
 *  @note Smallest positive number where 1.0f + FLOAT_EPSILON != 1.0f
 *  @note Value: 1.192092896e-07f (approximately 1.19 × 10^-7)
 *  @note Use for floating-point equality comparisons and numerical stability
 *  @example Use abs_f32(a - b) < FLOAT_EPSILON instead of a == b */
#define VKR_FLOAT_EPSILON 1.192092896e-07f

/** @brief Maximum value for float32_t
 *  @note Value: 3.402823466e+38f
 *  @note Use for bounds checking and initialization of maximum values
 *  @note Should be smaller than any realistic value in your application */
#define VKR_FLOAT_MAX 3.402823466e+38f

/** @brief Minimum value for float32_t
 *  @note Value: 1.175494351e-38f
 *  @note Use for bounds checking and initialization of minimum values
 *  @note Should be larger than any realistic value in your application */
#define VKR_FLOAT_MIN 1.175494351e-38f

vkr_global bool8_t vkr_rand_seeded = false;

/**
 * @brief Converts degrees to radians
 * @param degrees Angle value in degrees
 * @return Equivalent angle value in radians
 * @note Uses the precise conversion factor VKR_DEG2RAD_MULTIPLIER (π/180)
 */
vkr_internal INLINE float32_t vkr_to_radians(float32_t degrees) {
  return degrees * VKR_DEG2RAD_MULTIPLIER;
}

/**
 * @brief Converts radians to degrees
 * @param radians Angle value in radians
 * @return Equivalent angle value in degrees
 * @note Uses the precise conversion factor VKR_RAD2DEG_MULTIPLIER (180/π)
 */
vkr_internal INLINE float32_t vkr_to_degrees(float32_t radians) {
  return radians * VKR_RAD2DEG_MULTIPLIER;
}

/**
 * @brief Returns the minimum of two float32_t values
 * @param a First value to compare
 * @param b Second value to compare
 * @return The smaller of the two input values
 */
vkr_internal INLINE float32_t vkr_min_f32(float32_t a, float32_t b) {
  return (a < b) ? a : b;
}

/**
 * @brief Returns the maximum of two float32_t values
 * @param a First value to compare
 * @param b Second value to compare
 * @return The larger of the two input values
 */
vkr_internal INLINE float32_t vkr_max_f32(float32_t a, float32_t b) {
  return (a > b) ? a : b;
}

/**
 * @brief Returns the minimum of two float64_t values
 * @param a First value to compare
 * @param b Second value to compare
 * @return The smaller of the two input values
 */
vkr_internal INLINE float64_t vkr_min_f64(float64_t a, float64_t b) {
  return (a < b) ? a : b;
}

/**
 * @brief Returns the minimum of two uint64_t values
 * @param a First value to compare
 * @param b Second value to compare
 * @return The smaller of the two input values
 */
vkr_internal INLINE uint64_t vkr_min_u64(uint64_t a, uint64_t b) {
  return (a < b) ? a : b;
}

/**
 * @brief Returns the maximum of two float64_t values
 * @param a First value to compare
 * @param b Second value to compare
 * @return The larger of the two input values
 */
vkr_internal INLINE float64_t vkr_max_f64(float64_t a, float64_t b) {
  return (a > b) ? a : b;
}

/**
 * @brief Clamps a value between a minimum and maximum range
 * @param value The value to clamp
 * @param min_val The minimum allowed value (inclusive)
 * @param max_val The maximum allowed value (inclusive)
 * @return The input value clamped to [min_val, max_val]
 * @note If min_val > max_val, behavior is undefined
 * @example vkr_clamp_f32(150.0f, 0.0f, 100.0f) returns 100.0f
 */
vkr_internal INLINE float32_t vkr_clamp_f32(float32_t value, float32_t min_val,
                                            float32_t max_val) {
  return (value < min_val) ? min_val : (value > max_val) ? max_val : value;
}

/**
 * @brief Clamps a value between a minimum and maximum range
 * @param value The value to clamp
 * @param min_val The minimum allowed value (inclusive)
 * @param max_val The maximum allowed value (inclusive)
 * @return The input value clamped to [min_val, max_val]
 * @note If min_val > max_val, behavior is undefined
 */
vkr_internal INLINE float64_t vkr_clamp_f64(float64_t value, float64_t min_val,
                                            float64_t max_val) {
  return (value < min_val) ? min_val : (value > max_val) ? max_val : value;
}

/**
 * @brief Linear interpolation between two values
 * @param a Starting value (returned when t = 0.0)
 * @param b Ending value (returned when t = 1.0)
 * @param t Interpolation parameter, typically in range [0.0, 1.0]
 * @return Interpolated value between a and b
 * @note t values outside [0.0, 1.0] will extrapolate beyond the range [a, b]
 * @example vkr_lerp_f32(10.0f, 20.0f, 0.5f) returns 15.0f
 */
vkr_internal INLINE float32_t vkr_lerp_f32(float32_t a, float32_t b,
                                           float32_t t) {
  return a + t * (b - a);
}

/**
 * @brief Returns the absolute value of a float32_t
 * @param value Input value
 * @return Non-negative absolute value of the input
 * @note Uses the optimized fabsf() function from math.h
 */
vkr_internal INLINE float32_t vkr_abs_f32(float32_t value) {
  return fabsf(value);
}

/**
 * @brief Returns the sign of a float32_t value
 * @param value Input value
 * @return 1.0f if value >= 0, -1.0f if value < 0
 * @note Uses copysignf() for IEEE 754 compliant sign extraction
 * @note Returns 1.0f for +0.0f and -1.0f for -0.0f
 */
vkr_internal INLINE float32_t vkr_sign_f32(float32_t value) {
  return copysignf(1.0f, value);
}

/**
 * @brief Computes the square root of a float32_t value
 * @param value Input value (must be non-negative)
 * @return Square root of the input value
 * @note Behavior is undefined for negative inputs
 * @note Uses the optimized sqrtf() function from math.h
 */
vkr_internal INLINE float32_t vkr_sqrt_f32(float32_t value) {
  return sqrtf(value);
}

/**
 * @brief Computes the square root of a float64_t value
 * @param value Input value (must be non-negative)
 * @return Square root of the input value
 * @note Behavior is undefined for negative inputs
 * @note Uses the optimized sqrt() function from math.h
 */
vkr_internal INLINE float64_t vkr_sqrt_f64(float64_t value) {
  return sqrt(value);
}

/**
 * @brief Rounds a float32_t value down to the nearest integer
 * @param value Input floating-point value
 * @return Largest integer less than or equal to the input
 * @example vkr_floor_f32(3.7f) returns 3.0f, vkr_floor_f32(-2.3f) returns -3.0f
 */
vkr_internal INLINE float32_t vkr_floor_f32(float32_t value) {
  return floorf(value);
}

/**
 * @brief Rounds a float32_t value up to the nearest integer
 * @param value Input floating-point value
 * @return Smallest integer greater than or equal to the input
 * @example vkr_ceil_f32(3.2f) returns 4.0f, vkr_ceil_f32(-2.7f) returns -2.0f
 */
vkr_internal INLINE float32_t vkr_ceil_f32(float32_t value) {
  return ceilf(value);
}

/**
 * @brief Rounds a float32_t value to the nearest integer
 * @param value Input floating-point value
 * @return Nearest integer value (ties round away from zero)
 * @example vkr_round_f32(3.6f) returns 4.0f, vkr_round_f32(3.4f) returns 3.0f
 */
vkr_internal INLINE float32_t vkr_round_f32(float32_t value) {
  return roundf(value);
}

/**
 * @brief Raises a base to the power of an exponent
 * @param base The base value
 * @param exponent The exponent value
 * @return base raised to the power of exponent (base^exponent)
 * @note Special cases follow IEEE 754 standards (e.g., pow(0, 0) = 1)
 */
vkr_internal INLINE float32_t vkr_pow_f32(float32_t base, float32_t exponent) {
  return powf(base, exponent);
}

/**
 * @brief Computes the exponential function (e^x)
 * @param value The exponent value
 * @return e raised to the power of the input value
 * @note e ≈ 2.71828182845904523536
 */
vkr_internal INLINE float32_t vkr_exp_f32(float32_t value) {
  return expf(value);
}

/**
 * @brief Computes the natural logarithm (base e)
 * @param value Input value (must be positive)
 * @return Natural logarithm of the input value
 * @note Behavior is undefined for non-positive inputs
 */
vkr_internal INLINE float32_t vkr_log_f32(float32_t value) {
  return logf(value);
}

/**
 * @brief Computes the sine of an angle in radians
 * @param value Angle in radians
 * @return Sine of the input angle, range [-1.0, 1.0]
 */
vkr_internal INLINE float32_t vkr_sin_f32(float32_t value) {
  return sinf(value);
}

/**
 * @brief Computes the cosine of an angle in radians
 * @param value Angle in radians
 * @return Cosine of the input angle, range [-1.0, 1.0]
 */
vkr_internal INLINE float32_t vkr_cos_f32(float32_t value) {
  return cosf(value);
}

/**
 * @brief Computes the tangent of an angle in radians
 * @param value Angle in radians
 * @return Tangent of the input angle
 * @note Returns ±∞ for odd multiples of π/2
 */
vkr_internal INLINE float32_t vkr_tan_f32(float32_t value) {
  return tanf(value);
}

/**
 * @brief Computes the arc sine (inverse sine) in radians
 * @param value Input value, must be in range [-1.0, 1.0]
 * @return Arc sine of the input, range [-π/2, π/2]
 * @note Behavior is undefined for inputs outside [-1.0, 1.0]
 */
vkr_internal INLINE float32_t vkr_asin_f32(float32_t value) {
  return asinf(value);
}

/**
 * @brief Computes the arc cosine (inverse cosine) in radians
 * @param value Input value, must be in range [-1.0, 1.0]
 * @return Arc cosine of the input, range [0, π]
 * @note Behavior is undefined for inputs outside [-1.0, 1.0]
 */
vkr_internal INLINE float32_t vkr_acos_f32(float32_t value) {
  return acosf(value);
}

/**
 * @brief Computes the arc tangent (inverse tangent) in radians
 * @param value Input value
 * @return Arc tangent of the input, range [-π/2, π/2]
 */
vkr_internal INLINE float32_t vkr_atan_f32(float32_t value) {
  return atanf(value);
}

/**
 * @brief Copies the sign of one float32_t value to another
 * @param value Input value
 * @param sign Sign value to copy
 * @return The input value with the sign of the sign value
 */
vkr_internal INLINE float32_t vkr_copysign_f32(float32_t value,
                                               float32_t sign) {
#if defined(VKR_PLATFORM_WINDOWS)
  return _copysignf(value, sign);
#else
  return copysignf(value, sign);
#endif
}

/**
 * @brief Computes the arc tangent of y/x using the signs to determine quadrant
 * @param y Y-coordinate (numerator)
 * @param x X-coordinate (denominator)
 * @return Arc tangent of y/x, range [-π, π]
 * @note Handles all quadrants correctly, including when x = 0
 * @note Returns correct angles for (0,0), (+0,-0), etc.
 * @example vkr_atan2_f32(1.0f, 1.0f) returns π/4 (45°)
 */
vkr_internal INLINE float32_t vkr_atan2_f32(float32_t y, float32_t x) {
  return atan2f(y, x);
}

/**
 * @brief Generates a random float32_t value in the range [0.0, 1.0]
 * @return Random float32_t value between 0.0 (inclusive) and 1.0 (inclusive)
 * @note Uses the standard C rand() function, automatically seeds on first use
 * @note NOT thread-safe due to global state in rand()
 */
vkr_internal INLINE float32_t vkr_rand_f32() {
  return (float32_t)rand() / (float32_t)RAND_MAX;
}

/**
 * @brief Generates a random float32_t value within a specified range
 * @param min Minimum value (inclusive)
 * @param max Maximum value (inclusive)
 * @return Random float32_t value in the range [min, max]
 * @note If min > max, behavior is undefined
 * @note NOT thread-safe due to dependency on vkr_rand_f32()
 * @example vkr_rand_range_f32(1.5f, 10.5f) might return 7.23f
 */
vkr_internal INLINE float32_t vkr_rand_range_f32(float32_t min, float32_t max) {
  return min + vkr_rand_f32() * (max - min);
}

/**
 * @brief Divides a value by another value and rounds up to the nearest integer
 * @param a Dividend
 * @param b Divisor
 * @return Result of (a + b - 1u) / b
 */
vkr_internal INLINE uint32_t vkr_ceil_div_u32(uint32_t a, uint32_t b) {
  return (a + b - 1u) / b;
}

/**
 * @brief Aligns a value up to the nearest multiple of another value
 * @param v Value to align
 * @param a Alignment value
 * @return Value aligned up to the nearest multiple of a
 */
vkr_internal INLINE uint32_t vkr_align_up_u32(uint32_t v, uint32_t a) {
  uint32_t m = a - 1u;
  return (v + m) & ~m;
}

/**
 * @brief Returns the number of decimal digits in a uint32_t value
 * @param x Input value
 * @return Number of decimal digits in the input value
 */
vkr_internal INLINE uint32_t vkr_dec_digits_u32(uint32_t x) {
  if (x >= 1000000000u)
    return 10u;
  if (x >= 100000000u)
    return 9u;
  if (x >= 10000000u)
    return 8u;
  if (x >= 1000000u)
    return 7u;
  if (x >= 100000u)
    return 6u;
  if (x >= 10000u)
    return 5u;
  if (x >= 1000u)
    return 4u;
  if (x >= 100u)
    return 3u;
  if (x >= 10u)
    return 2u;
  return 1u;
}

/**
 * @brief Writes a uint32_t value to a string as decimal digits without leading
 * zeros
 * @param p Pointer to the string
 * @param v Value to write
 * @return Pointer to the next character after the written digits
 */
vkr_internal INLINE char *vkr_write_u32_dec(char *p, uint32_t v) {
  // write decimal digits without leading zeros
  char buf[10];
  uint32_t n = 0;
  do {
    buf[n++] = (char)('0' + (v % 10u));
    v /= 10u;
  } while (v);
  // reverse into p
  for (uint32_t i = 0; i < n; ++i)
    p[i] = buf[n - 1 - i];
  return p + n;
}

/**
 * @brief Generates a random int32_t value
 * @return Random int32_t value in the range [0, RAND_MAX]
 * @note Automatically seeds the random number generator on first call
 * @note Uses vkr_platform_get_absolute_time() for seeding to ensure uniqueness
 * @note NOT thread-safe due to global state in rand() and seeding logic
 */
vkr_internal INLINE int32_t vkr_rand_i32() {
  if (!vkr_rand_seeded) {
    srand((int32_t)vkr_platform_get_absolute_time());
    vkr_rand_seeded = true;
  }

  return rand();
}

/**
 * @brief Generates a random int32_t value within a specified range
 * @param min Minimum value (inclusive)
 * @param max Maximum value (inclusive)
 * @return Random int32_t value in the range [min, max]
 * @note If min > max, behavior is undefined
 * @note NOT thread-safe due to dependency on vkr_rand_i32()
 * @example vkr_rand_range_i32(1, 6) simulates a dice roll (returns 1-6)
 */
vkr_internal INLINE int32_t vkr_rand_range_i32(int32_t min, int32_t max) {
  return (vkr_rand_i32() % (max - min + 1)) + min;
}