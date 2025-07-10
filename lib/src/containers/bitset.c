#include "bitset.h"

Bitset8 bitset8_create() { return (Bitset8){0}; }

void bitset8_set(Bitset8 *bitset, uint8_t flag) {
  assert((flag & (flag - 1)) == 0 && flag != 0 && flag <= 0x80 &&
         "Flag must be a single power of 2 within 8-bit range");
  assert(bitset != NULL && "Bitset must not be NULL");
  bitset->set |= flag; // Use bitwise OR to set the flag
}

void bitset8_clear(Bitset8 *bitset, uint8_t flag) {
  assert((flag & (flag - 1)) == 0 && flag != 0 && flag <= 0x80 &&
         "Flag must be a single power of 2 within 8-bit range");
  assert(bitset != NULL && "Bitset must not be NULL");
  bitset->set &= ~flag; // Use bitwise AND with the inverse of the flag
}

void bitset8_toggle(Bitset8 *bitset, uint8_t flag) {
  assert((flag & (flag - 1)) == 0 && flag != 0 && flag <= 0x80 &&
         "Flag must be a single power of 2 within 8-bit range");
  assert(bitset != NULL && "Bitset must not be NULL");
  bitset->set ^= flag; // Use bitwise XOR to toggle the flag
}

bool32_t bitset8_is_set(const Bitset8 *bitset, uint8_t flag) {
  assert((flag & (flag - 1)) == 0 && flag != 0 && flag <= 0x80 &&
         "Flag must be a single power of 2 within 8-bit range");
  assert(bitset != NULL && "Bitset must not be NULL");
  return (bitset->set & flag) !=
         0; // Use bitwise AND to check if the flag is set
}

uint8_t bitset8_get_value(const Bitset8 *bitset) {
  assert(bitset != NULL && "Bitset must not be NULL");
  return bitset->set;
}