#include <stddef.h>

#include "ffi_func.h"

int main(void) {
  struct ffi_type arg1 = {0, 1, {NULL, NULL, NULL, (struct ffi_type *)0xFFFFFFFF}};
  arg1.a = 1;
  arg1.b = 2;
  ffi_func_1(arg1, 3);
  struct ffi_type result = ffi_func_2(arg1, 4);
  return result.a + result.b;
}
