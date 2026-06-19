#include <stddef.h>
#include <stdint.h>

/* Global struct (promoted in-place) */
struct GlobalPoint {
  int x;
  int y;
  float z;
};

/* Pointer-only struct (must be IGNORED) */
struct PtrOnly {
  int value;
};

/* Bit-field struct */
struct BitfieldFlags {
  unsigned int active : 1;
  unsigned int mode : 3;
  int level : 12;
};

/* Mixed struct (array and pointer fields left alone) */
struct MixedStruct {
  short count;     /* scalar  - promoted                   */
  char name[64];   /* array   - left alone                 */
  int *ptr;        /* pointer - left alone                 */
  unsigned int id; /* scalar  - promoted                   */
  char grid[4][8]; /* 2-D array - left alone               */
};

/* Deeply nested struct with anonymous inner union */
struct Nested {
  int tag; /* scalar - promoted                   */
  union {  /* anonymous union - rendered inline   */
    int as_int;
    float as_float;
  };
  struct { /* anonymous struct - rendered inline  */
    short lo;
    short hi;
  } pair;
};

/* ======================================================================== */

void use_global(void) {
  struct GlobalPoint p = {1, 2, 3.0f};
  (void)p;
}

void use_pointer_only(void) {
  /* PtrOnly is only ever used via pointer - must NOT be promoted */
  struct PtrOnly *p = 0;
  (void)p;
}

void use_bitfield(void) {
  struct BitfieldFlags f = {1, 2, -4};
  (void)f;
}

void use_mixed(void) {
  struct MixedStruct m = {3, "hello", 0, 42u};
  (void)m;
}

void use_nested(void) {
  struct Nested n = {7, {.as_int = 99}, {1, 2}};
  (void)n;
}

void use_local_rect(void) {
  /* LocalRect is defined inside a function, must be lifted to global scope */
  struct LocalRect {
    short width;
    short height;
  };
  struct LocalRect r = {100, 200};
  (void)r;

  struct MixedStruct2 {
    short count;
    char name[64];
    int *ptr;
    unsigned int id;
    char grid[4][8];
  };

  struct MixedStruct2 m2[5];
  m2[0].count = 10;
  (void)m2;

  struct Nested2 {
    int tag;
    union {
      int as_int;
      float as_float;
    };
    struct {
      short lo;
      short hi;
    } pair;
  };
  struct Nested2 n2[2];
  n2[0].tag = 42;
  (void)n2;
}

int main(void) {
  use_global();
  use_pointer_only();
  use_bitfield();
  use_mixed();
  use_nested();
  use_local_rect();
  return 0;
}
