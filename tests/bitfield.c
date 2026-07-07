#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct bitfieldtest {
  uint64_t a : 1;
  uint64_t b : 2;
  uint64_t c : 3;
  uint64_t d : 63;
  uint64_t e : 4;
  uint64_t padding : 128 - (1 + 2 + 3 + 63 + 4);
};

void main2() {
  struct bitfieldtest bf;
  memset(&bf, 0, sizeof(bf));
  struct bitfieldtest *pbf = &bf;
  assert(pbf->a == 0);
  assert(pbf->b == 0);
  assert(pbf->c == 0);
  assert(pbf->d == 0);
  assert(pbf->e == 0);

  pbf->a = 1;
  pbf->b = 1;
  pbf->c = 6;
  pbf->d = 0x7000'0000'0000'000F;
  pbf->e = 4;

  assert(pbf->a == 1);
  assert(pbf->b == 1);
  assert(pbf->c == 6);
  assert(pbf->d == 0x7000'0000'0000'000F);
  assert(pbf->e == 4);

  printf("a=%llu b=%llu c=%llu d=%llu e=%llu\n", pbf->a, pbf->b, pbf->c, pbf->d, pbf->e);

  assert(pbf->a == 1);
  assert(pbf->b == 1);
  assert(pbf->c == 6);
  assert(pbf->d == 0x7000'0000'0000'000F);
  assert(pbf->e == 4);
}

int main(void) {
  struct bitfieldtest bf;
  memset(&bf, 0, sizeof(bf));
  assert(bf.a == 0);
  assert(bf.b == 0);
  assert(bf.c == 0);
  assert(bf.d == 0);
  assert(bf.e == 0);

  bf.a = 1;
  bf.b = 1;
  bf.c = 6;
  bf.d = 0x7000'0000'0000'000F;
  bf.e = 4;

  assert(bf.a == 1);
  assert(bf.b == 1);
  assert(bf.c == 6);
  assert(bf.d == 0x7000'0000'0000'000F);
  assert(bf.e == 4);

  printf("a=%llu b=%llu c=%llu d=%llu e=%llu\n", bf.a, bf.b, bf.c, bf.d, bf.e);

  assert(bf.a == 1);
  assert(bf.b == 1);
  assert(bf.c == 6);
  assert(bf.d == 0x7000'0000'0000'000F);
  assert(bf.e == 4);

  main2();

  return 0;
}
