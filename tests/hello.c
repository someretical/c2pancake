#include <stdio.h>

int add(int a, int b) { return a + b; }

int main() {
  // @ffi: print_hello
  printf("Hello World\n");

  int sum = add(3, 4);
  int result = sum + 10;

  // @ffi: print_done
  printf("Done: %d\n", result);

  return 0;
}
