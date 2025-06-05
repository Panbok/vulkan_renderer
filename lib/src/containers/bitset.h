/**
 * @file bitset.h
 * @brief A simple bitset implementation.
 *
 * This implementation is a simple bitset implementation that allows you to set,
 * clear, and toggle bits.
 */

#pragma once

#include "defines.h"
#include "platform/platform.h"

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
bool32_t bitset8_is_set(Bitset8 *bitset, uint8_t flag);

/**
 * @brief Returns the current value of the bitset as an unsigned 8-bit integer.
 * @param bitset A pointer to the Bitset8 instance to get the value from.
 * @return The current value of the bitset as an unsigned 8-bit integer.
 */
uint8_t bitset8_get_value(const Bitset8 *bitset);

// --- Example Usage ---
/*

// 1. Define your flags using an enum (or #defines)
//    The values MUST be powers of 2 (single bit set) using bit shifts.
typedef enum {
    ENTITY_FLAG_NONE    = 0,      // Useful for representing no flags
    ENTITY_FLAG_ACTIVE  = 1 << 0, // Bit 0 (Value 1)
    ENTITY_FLAG_VISIBLE = 1 << 1, // Bit 1 (Value 2)
    ENTITY_FLAG_STATIC  = 1 << 2, // Bit 2 (Value 4)
    ENTITY_FLAG_PLAYER  = 1 << 3, // Bit 3 (Value 8)
    // ... up to ENTITY_FLAG_BIT_7 = 1 << 7 (Value 128)
} EntityFlags;


#include <stdio.h> // For printf in example

int main() {
    // Create a bitset
    Bitset8 entity_status = bitset8_create();
    printf("Initial value: %u\n", bitset8_get_value(&entity_status)); // Output:
0

    // Set some flags
    bitset8_set(&entity_status, ENTITY_FLAG_ACTIVE);
    bitset8_set(&entity_status, ENTITY_FLAG_VISIBLE);
    printf("After set ACTIVE | VISIBLE: %u\n",
bitset8_get_value(&entity_status)); // Output: 3 (1 | 2)

    // Check flags
    if (bitset8_is_set(&entity_status, ENTITY_FLAG_ACTIVE)) {
        printf("Entity is ACTIVE\n");
    }
    if (!bitset8_is_set(&entity_status, ENTITY_FLAG_STATIC)) {
        printf("Entity is NOT STATIC\n");
    }

    // Toggle a flag
    printf("Toggling VISIBLE...\n");
    bitset8_toggle(&entity_status, ENTITY_FLAG_VISIBLE); // Turn VISIBLE off
    printf("After toggle VISIBLE: %u\n", bitset8_get_value(&entity_status)); //
Output: 1 if (!bitset8_is_set(&entity_status, ENTITY_FLAG_VISIBLE)) {
        printf("Entity is now NOT VISIBLE\n");
    }
    bitset8_toggle(&entity_status, ENTITY_FLAG_VISIBLE); // Turn VISIBLE back on
    printf("After toggle VISIBLE again: %u\n",
bitset8_get_value(&entity_status)); // Output: 3

    // Clear a flag
    printf("Clearing ACTIVE...\n");
    bitset8_clear(&entity_status, ENTITY_FLAG_ACTIVE);
    printf("After clear ACTIVE: %u\n", bitset8_get_value(&entity_status)); //
Output: 2

    return 0;
}

*/