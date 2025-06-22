/**
 * @file math_defines.h
 * @brief Mathematical constants and conversion factors for high-precision
 * computation
 *
 * This file provides essential mathematical constants with high precision
 * suitable for graphics programming, game development, and scientific
 * computation. All constants use the `float32_t` precision and are carefully
 * chosen to provide maximum accuracy within the 32-bit floating-point
 * representation.
 *
 * Constant Categories:
 *
 * 1. **Fundamental Constants**: Core mathematical constants
 *    - PI and related values (PI_2, HALF_PI, QUARTER_PI)
 *    - Reciprocal values (ONE_OVER_PI, ONE_OVER_TWO_PI)
 *
 * 2. **Square Root Constants**: Common square root values
 *    - SQRT_TWO, SQRT_THREE
 *    - SQRT_ONE_OVER_TWO, SQRT_ONE_OVER_THREE
 *
 * 3. **Angle Conversion**: Precise conversion multipliers
 *    - DEG2RAD_MULTIPLIER, RAD2DEG_MULTIPLIER
 *
 * 4. **Time Conversion**: Time unit conversion multipliers
 *    - SEC_TO_MS_MULTIPLIER, MS_TO_SEC_MULTIPLIER
 *
 * 5. **Numerical Limits**: Special values for numerical computation
 *    - INFINITY, FLOAT_EPSILON
 *
 * Precision Notes:
 * - All constants are defined with maximum precision for float32_t
 * - PI uses 17 significant digits for optimal accuracy
 * - Square root constants use extended precision values
 * - Conversion factors are computed at compile-time for efficiency
 *
 * Usage Examples:
 * ```c
 * // Angle conversions
 * float32_t rad = 45.0f * DEG2RAD_MULTIPLIER;  // Convert 45° to radians
 * float32_t deg = PI * RAD2DEG_MULTIPLIER;     // Convert π to degrees
 *
 * // Circle calculations
 * float32_t circumference = 2.0f * PI * radius;
 * float32_t area = PI * radius * radius;
 *
 * // Time conversions
 * float32_t ms = seconds * SEC_TO_MS_MULTIPLIER;
 * float32_t sec = milliseconds * MS_TO_SEC_MULTIPLIER;
 *
 * // Numerical comparisons
 * bool32_t nearly_equal = abs_f32(a - b) < FLOAT_EPSILON;
 * ```
 */

#pragma once

/** @brief Mathematical constant π (pi) with high precision
 *  @note Value: 3.14159265358979323846...
 *  @note Uses 17 significant digits for optimal float32_t accuracy */
#define PI 3.14159265358979323846f

/** @brief Mathematical constant 2π (two pi)
 *  @note Useful for complete circle calculations
 *  @note Computed as 2.0 * PI for compile-time evaluation */
#define PI_2 2.0f * PI

/** @brief Mathematical constant π/2 (half pi)
 *  @note Represents 90 degrees in radians
 *  @note Computed as 0.5 * PI for compile-time evaluation */
#define HALF_PI 0.5f * PI

/** @brief Mathematical constant π/4 (quarter pi)
 *  @note Represents 45 degrees in radians
 *  @note Computed as 0.25 * PI for compile-time evaluation */
#define QUARTER_PI 0.25f * PI

/** @brief Reciprocal of π (1/π)
 *  @note Useful for avoiding division by PI in calculations
 *  @note Computed as 1.0 / PI for compile-time evaluation */
#define ONE_OVER_PI 1.0f / PI

/** @brief Reciprocal of 2π (1/(2π))
 *  @note Useful for frequency calculations and normalization
 *  @note Computed as 1.0 / PI_2 for compile-time evaluation */
#define ONE_OVER_TWO_PI 1.0f / PI_2

/** @brief Square root of 2 (√2) with high precision
 *  @note Value: 1.41421356237309504880...
 *  @note Useful for diagonal calculations and normalization */
#define SQRT_TWO 1.41421356237309504880f

/** @brief Square root of 3 (√3) with high precision
 *  @note Value: 1.73205080756887729352...
 *  @note Useful for triangular and hexagonal calculations */
#define SQRT_THREE 1.73205080756887729352f

/** @brief Square root of 1/2 (1/√2) with high precision
 *  @note Value: 0.70710678118654752440...
 *  @note Equivalent to √2/2, useful for 45-degree rotations */
#define SQRT_ONE_OVER_TWO 0.70710678118654752440f

/** @brief Square root of 1/3 (1/√3) with high precision
 *  @note Value: 0.57735026918962576450...
 *  @note Equivalent to √3/3, useful for equilateral triangle calculations */
#define SQRT_ONE_OVER_THREE 0.57735026918962576450f

/** @brief Conversion multiplier from degrees to radians
 *  @note Multiply degrees by this value to get radians
 *  @note Computed as PI / 180.0 for compile-time evaluation
 *  @example 45.0f * DEG2RAD_MULTIPLIER = π/4 radians */
#define DEG2RAD_MULTIPLIER PI / 180.0f

/** @brief Conversion multiplier from radians to degrees
 *  @note Multiply radians by this value to get degrees
 *  @note Computed as 180.0 / PI for compile-time evaluation
 *  @example PI * RAD2DEG_MULTIPLIER = 180.0f degrees */
#define RAD2DEG_MULTIPLIER 180.0f / PI

/** @brief Conversion multiplier from seconds to milliseconds
 *  @note Multiply seconds by this value to get milliseconds
 *  @note Value: 1000.0f (1 second = 1000 milliseconds) */
#define SEC_TO_MS_MULTIPLIER 1000.0f

/** @brief Conversion multiplier from milliseconds to seconds
 *  @note Multiply milliseconds by this value to get seconds
 *  @note Value: 0.001f (1 millisecond = 0.001 seconds) */
#define MS_TO_SEC_MULTIPLIER 0.001f

/** @brief Large value representing positive infinity for float32_t
 *  @note Value: 1e30f (1 × 10^30)
 *  @note Use for bounds checking and initialization of minimum values
 *  @note Should be larger than any realistic value in your application */
#define INFINITY 1e30f

/** @brief Machine epsilon for float32_t precision
 *  @note Smallest positive number where 1.0f + FLOAT_EPSILON != 1.0f
 *  @note Value: 1.192092896e-07f (approximately 1.19 × 10^-7)
 *  @note Use for floating-point equality comparisons and numerical stability
 *  @example Use abs_f32(a - b) < FLOAT_EPSILON instead of a == b */
#define FLOAT_EPSILON 1.192092896e-07f