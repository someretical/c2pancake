#include <stdio.h>

void ffiprint_hello(unsigned char *c, long clen, unsigned char *a, long alen) {
  printf("Hello from Pancake!\n");
  fflush(stdout);
}

void ffiprint_done(unsigned char *c, long clen, unsigned char *a, long alen) {
  printf("Done! Program completed successfully.\n");
  fflush(stdout);
}
