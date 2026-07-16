#include <stdint.h>

static inline uint64_t __c2pnk_get_bit_u64(uint64_t value, uint64_t bit) { return ((value >> bit) & 1ULL) != 0; }

static inline void __c2pnk_set_bit(uint8_t *byte, uint64_t bit) {
  uint64_t val = (uint64_t)*byte;
  // truncation
  *byte = (uint8_t)(val | (uint64_t)(1ULL << bit));
}

static inline void __c2pnk_clear_bit(uint8_t *byte, uint64_t bit) {
  uint64_t val = (uint64_t)*byte;
  // truncation
  *byte = (uint8_t)(val & ~(1ULL << bit));
}

void __c2pnk_set_bitfield_u64(uint64_t value, uint8_t *field, uint64_t lhs_bit, uint64_t rhs_bit) {
  uint64_t width = rhs_bit - lhs_bit + 1;

  uint64_t i = 0;
  while (i < width) {
    uint64_t bit_index = lhs_bit + i;
    uint8_t *byte = &field[bit_index >> 3]; // / 8

    uint64_t cond = __c2pnk_get_bit_u64(value, i);
    uint64_t index = bit_index & 7; // % 8
    if (cond) {
      __c2pnk_set_bit(byte, index);
    } else {
      __c2pnk_clear_bit(byte, index);
    }

    i = i + 1;
  }
}

uint64_t __c2pnk_get_bitfield_u64(const uint8_t *field, uint64_t lhs_bit, uint64_t rhs_bit) {
  uint64_t value = 0;
  uint64_t width = rhs_bit - lhs_bit + 1;

  uint64_t i = 0;
  while (i < width) {
    uint64_t bit_index = lhs_bit + i;

    uint64_t byte = (uint64_t)field[bit_index >> 3]; // / 8
    uint64_t index = bit_index & 7;                  // % 8
    uint64_t mask = (uint64_t)(1ULL << index);

    if (byte & mask) {
      value = value | (1ULL << i);
    }

    i = i + 1;
  }

  return value;
}

int64_t __c2pnk_get_bitfield_i64(const uint8_t *field, uint64_t lhs_bit, uint64_t rhs_bit) {
  uint64_t value = __c2pnk_get_bitfield_u64(field, lhs_bit, rhs_bit);

  uint64_t width = rhs_bit - lhs_bit + 1;

  /* manual sign-extend if the extracted field is narrower than 64 bits */
  if (width < 64) {
    uint64_t sign = 1ULL << (width - 1);
    return (int64_t)((value ^ sign) - sign);
  }

  return (int64_t)value;
}

void __c2pnk_set_bitfield_i64(int64_t value, uint8_t *field, uint64_t lhs_bit, uint64_t rhs_bit) {
  __c2pnk_set_bitfield_u64((uint64_t)value, field, lhs_bit, rhs_bit);
}