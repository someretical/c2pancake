#include <stdint.h>
#include <string.h>
#include <unistd.h>

void main5(void) { /* does nothing but should have return type modified */ }

int main(void) {
  const char *a = "hello world\n";
  write(1, a, 12);
  return 0;
}
