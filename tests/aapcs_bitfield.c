#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct R {
  volatile int a : 11;
  volatile int b : 1;
  volatile int c : 3;
  volatile int d : 16;
  volatile int e : 1;
};

int main(void) {
  struct R *pbf = (struct R *)malloc(sizeof(struct R));
  memset(pbf, 0, sizeof(struct R));

  pbf->a = -864;
  pbf->b = -1;
  pbf->c = -2;
  pbf->d = 24000;
  pbf->e = -1;

  int64_t a = pbf->a;
  assert(a == -864);
  int64_t b = pbf->b;
  assert(b == -1);
  int64_t c = pbf->c;
  assert(c == -2);
  int64_t d = pbf->d;
  assert(d == 24000);
  int64_t e = pbf->e;
  assert(e == -1);

  return 0;
}
