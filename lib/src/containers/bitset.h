/**
 * @file bitset.h
 * @brief A simple bitset implementation.
 *
 * This implementation is a simple bitset implementation that allows you to set,
 * clear, and toggle bits.
 *
 * IMPORTANT WARNING FOR TYPEDEF USAGE:
 * ====================================
 *
 * When using Bitset8 as a typedef (e.g., typedef Bitset8 MyFlags), you MUST
 * remember that Bitset8 is a STRUCTURE, not a raw integer!
 *
 * ❌ WRONG: Directly assigning integer flags to bitset fields or parameters
 *   MyFlags flags = FLAG_A | FLAG_B;  // This assigns an int to a struct!
 *
 * ✅ CORRECT: Always use helper functions to create bitsets from integer flags
 *   MyFlags flags = my_flags_from_bits(FLAG_A | FLAG_B);
 *
 * For any typedef'd Bitset8 type, you should provide helper functions like:
 * - MyFlags my_flags_create(void) { return bitset8_create(); }
 * - MyFlags my_flags_from_bits(uint8_t bits) { ... }
 * - void my_flags_add(MyFlags *flags, uint8_t flag) { bitset8_set(flags, flag);
 * }
 *
 * See examples in renderer.h (ShaderStageFlags) and buffer.h
 * (VertexArrayFromMeshOptions).
 */

#pragma once

#include "defines.h"

typedef struct Bitset8 {
  uint8_t set;
} Bitset8;

/**
 * @brief Creates and initializes a new 8-bit bitset with all bits set to 0.
 * @return A new Bitset8 instance.
 */
Bitset8 bitset8_create();

/**
 * @brief Sets (turns on) the specified flag bit(s) in the bitset.
 * @param bitset A pointer to the Bitset8 instance to modify.
 * @param flag The flag bit(s) to set (e.g., 1 << 2). This can be a single
 *             flag or multiple flags OR'd together.
 */
void bitset8_set(Bitset8 *bitset, uint8_t flag);

/**
 * @brief Clears (turns off) the specified flag bit(s) in the bitset.
 * @param bitset A pointer to the Bitset8 instance to modify.
 * @param flag The flag bit(s) to clear (e.g., 1 << 2). This can be a single
 *             flag or multiple flags OR'd together.
 */
void bitset8_clear(Bitset8 *bitset, uint8_t flag);

/**
 * @brief Toggles (flips) the specified flag bit(s) in the bitset.
 *        If a bit in 'flag' is set, the corresponding bit in 'bitset' is
 * flipped.
 * @param bitset A pointer to the Bitset8 instance to modify.
 * @param flag The flag bit(s) to toggle (e.g., 1 << 2). This can be a single
 *             flag or multiple flags OR'd together.
 */
void bitset8_toggle(Bitset8 *bitset, uint8_t flag);

/**
 * @brief Checks if the specified flag bit is set (turned on) in the bitset.
 * @param bitset A pointer to the Bitset8 instance to check (const).
 * @param flag The single flag bit to check (e.g., 1 << 2). Should represent
 *             only one bit for unambiguous checking.
 * @return true if the specified flag bit is set, false otherwise.
 */
bool32_t bitset8_is_set(const Bitset8 *bitset, uint8_t flag);

/**
 * @brief Returns the current value of the bitset as an unsigned 8-bit integer.
 * @param bitset A pointer to the Bitset8 instance to get the value from.
 * @return The current value of the bitset as an unsigned 8-bit integer.
 */
uint8_t bitset8_get_value(const Bitset8 *bitset);