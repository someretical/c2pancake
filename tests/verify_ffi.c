#include <stdio.h>
#include <stdlib.h>

void ffireport_pass(unsigned char *c, long clen, unsigned char *a, long alen) {
  printf("PASS: All tests passed!\n");
  fflush(stdout);
}

void ffireport_fail(unsigned char *c, long clen, unsigned char *a, long alen) {
  printf("FAIL: Some tests failed\n");
  fflush(stdout);
  exit(1);
}
