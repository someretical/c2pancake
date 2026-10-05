#include <stdint.h>

// void main5(void) { /* does nothing but should have return type modified */ }

// int main(void) {
//   int a = 7, b = 5, c;
//   main5();

//   uint64_t x[10] = {0UL};

//   x[0UL] = 1UL;
//   x[1UL] = 2UL;

//   return 0;
// }

uint64_t __c2pnk_local_x_main_L288C3_25_0_3[10];
/* c2pancake: promoted variable declarations for function main END */
int main(void) {
  *(__c2pnk_local_x_main_L288C3_25_0_3 + 0UL) = 0UL;

  *(__c2pnk_local_x_main_L288C3_25_0_3 + 0UL) = 1UL;
  *(__c2pnk_local_x_main_L288C3_25_0_3 + 1UL) = 2UL;

  return (int)(0UL);
}