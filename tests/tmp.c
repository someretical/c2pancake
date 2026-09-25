#include <stdint.h>

void main5(void) { /* does nothing but should have return type modified */ }

int main(void) {
  int a = 7, b = 5, c;
  main5();

  uint64_t x[10] = {0UL};

  x[0UL] = 1UL;
  x[1UL] = 2UL;

  return 0;
}