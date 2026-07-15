#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct bitfieldtest {
  uint64_t a : 23;
  uint64_t b : 45;
  uint64_t c : 12;
  uint64_t d : 54;
  uint64_t e : 6;
};

int main(void) {
  struct bitfieldtest bf;
  memset(&bf, 0, sizeof(bf));
  struct bitfieldtest *pbf = &bf;
  // assert(pbf->a == 0);
  // assert(pbf->b == 0);
  // assert(pbf->c == 0);
  // assert(pbf->d == 0);
  // assert(pbf->e == 0);

  pbf->a = 8200709;           // max 8388608
  pbf->b = 35184372080000;    // max 35184372088831
  pbf->c = 2657;              // max 4095
  pbf->d = 18014398509400000; // max 18014398509481983
  pbf->e = 44;                // max 63

  uint64_t a = pbf->a;
  assert(a == 8200709);
  uint64_t b = pbf->b;
  assert(b == 35184372080000);
  uint64_t c = pbf->c;
  assert(c == 2657);
  uint64_t d = pbf->d;
  assert(d == 18014398509400000);
  uint64_t e = pbf->e;
  assert(e == 44);

  // pbf->a = 1;
  // pbf->b = 1;
  // pbf->c = 6;
  // pbf->d = 0x7000'0000'0000'000F;
  // pbf->e = 4;

  // assert(pbf->a == 1);
  // assert(pbf->b == 1);
  // assert(pbf->c == 6);
  // assert(pbf->d == 0x7000'0000'0000'000F);
  // assert(pbf->e == 4);

  // printf("a=%d b=%d c=%d d=%lu e=%d\n", pbf->a, pbf->b, pbf->c, pbf->d, pbf->e);

  // assert(pbf->a == 1);
  // assert(pbf->b == 1);
  // assert(pbf->c == 6);
  // assert(pbf->d == 0x7000'0000'0000'000F);
  // assert(pbf->e == 4);
}

// int main(void) {
//   struct bitfieldtest bf;
//   memset(&bf, 0, sizeof(bf));
//   assert(bf.a == 0);
//   assert(bf.b == 0);
//   assert(bf.c == 0);
//   assert(bf.d == 0);
//   assert(bf.e == 0);

//   bf.a = 1;
//   bf.b = 1;
//   bf.c = 6;
//   bf.d = 0x7000'0000'0000'000F;
//   bf.e = 4;

//   assert(bf.a == 1);
//   assert(bf.b == 1);
//   assert(bf.c == 6);
//   assert(bf.d == 0x7000'0000'0000'000F);
//   assert(bf.e == 4);

//   printf("a=%d b=%d c=%d d=%lu e=%d\n", bf.a, bf.b, bf.c, bf.d, bf.e);

//   assert(bf.a == 1);
//   assert(bf.b == 1);
//   assert(bf.c == 6);
//   assert(bf.d == 0x7000'0000'0000'000F);
//   assert(bf.e == 4);

//   main2();

//   return 0;
// }
