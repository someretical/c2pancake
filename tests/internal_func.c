#include "internal_func.h"

#include <stdint.h>

// this function should not be rewritten at ALL
uintptr_t compatible_func(uintptr_t a, uintptr_t b) {
  a = a + 1UL;
  return a + b;
}

// this function should not be rewritten either
uintptr_t compatible_func2(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5) {
  a1 = a1 + 1UL;
  return a1 + a2 + a3 + a4 + a5;
}

// this function should not be rewritten at ALL
uintptr_t compatible_func3(void) { return 1UL; }

// this function should be rewritten to return a unsigned long
// it should be annotated with the __c2pnk_rewritten_non_ffi_function attribute
void compatible_func4(uintptr_t a1, uintptr_t a2) { (void)(a1 + a2); }

// this function should be rewritten to return a unsigned long
// it should be annotated with the __c2pnk_rewritten_non_ffi_function attribute
void compatible_func5(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5) {
  (void)(a1 + a2 + a3 + a4 + a5);
  return;
}

struct helper1 {
  uintptr_t a;
  uintptr_t b;
};
// this function should use a global pancake var for the return value since it doesn't fit in a word
// the inputs should not be hoisted since they fit in a word, but the return value should be hoisted
struct helper1 compatible_func100(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5) {
  struct helper1 h;
  h.a = a1 + 1UL;
  h.b = a2 + a3 + a4 + a5;
  return h;
}

// this function should use a global pancake var for the return value since it doesn't fit in a word
// the inputs should also be hoisted since they don't fit in a word
// in the future, this can be optimized if the struct can be easily converted into a pancake shape
struct helper1 compatible_func101(struct helper1 h1, struct helper1 h2, struct helper1 h3, struct helper1 h4,
                                  struct helper1 h5) {
  struct helper1 h;
  h.a = h1.a + 1UL;
  h.b = h2.b + h3.b + h4.b + h5.b;
  return h;
}

int main(void) {
  uintptr_t v1 = 1UL;
  uintptr_t v2 = 2UL;
  uintptr_t v3 = compatible_func(v1, v2);
  (void)v3;
  uintptr_t v4 = compatible_func2(v1, v2, v3, v1, v2);
  (void)v4;
  uintptr_t v5 = compatible_func3();
  (void)v5;
  compatible_func5(v1, v2, v3, v4, v5);

  struct helper1 h = compatible_func100(v1, v2, v3, v4, v1);
  (void)h;
  struct helper1 h2 = compatible_func101(h, h, h, h, h);
  (void)h2;

  uintptr_t v100 = external_func1(v1, v2, v3);
  (void)v100;
  uintptr_t v101 = external_func2(v1, v2, v3, v4, v100);
  (void)v101;
  uintptr_t v102 = external_func3();
  (void)v102;
  external_func4(v1, v2);
  external_func5(v1, v2, v3, v4, v5);

  struct external_helper h3 = external_func100(v1, v2, v3, v4, v1);
  (void)h3;
  struct external_helper h4 = external_func101(h3, h3, h3, h3, h3);
  (void)h4;
  external_func102(h3, h3, h3, h3, h3);

  return 0UL;
}