#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct d {
  uint64_t v;
  uint64_t u;
};

struct c {
  uint64_t z;
  struct d *d;
};

struct z {
  uint64_t w;
  struct c *c;
};

struct b {
  uint64_t y;
  struct z z;
};

struct a {
  uint64_t x;
  struct b *b;
};

char *itoa_no_div(uint64_t value, char *buf) {
  uint64_t n;
  char *p = buf;

  if (value < 0) {
    *p++ = '-';
    n = -(uint64_t)value;
  } else {
    n = value;
  }

  uint64_t place = 1000000000;
  uint64_t started = 0;

  for (;;) {
    uint64_t digit = 0;

    while (n >= place) {
      n -= place;
      digit++;
    }

    if (digit || started || place == 1) {
      *p++ = '0' + digit;
      started = 1;
    }

    if (place == 1) {
      break;
    }

    if (place == 1000000000)
      place = 100000000;
    else if (place == 100000000)
      place = 10000000;
    else if (place == 10000000)
      place = 1000000;
    else if (place == 1000000)
      place = 100000;
    else if (place == 100000)
      place = 10000;
    else if (place == 10000)
      place = 1000;
    else if (place == 1000)
      place = 100;
    else if (place == 100)
      place = 10;
    else
      place = 1;
  }

  *p = '\0';
  return buf;
}

uint64_t main(void) {
  struct d d;
  d.v = 16;
  d.u = 42;

  struct c c;
  c.z = 7;
  c.d = &d;

  struct b b;
  b.y = 3;
  b.z.w = 5;
  b.z.c = &c;

  struct a a;
  a.x = 67;
  a.b = &b;

  a.b->z.c->d->v = 10;
  a.b->z.c->d->u++;

  uint64_t abc[10];
  abc[0] = 1;
  abc[1] = 2;
  abc[2] = 3;
  abc[3] = 4;
  abc[4] = 5;
  abc[5] = 6;
  abc[6] = 7;
  abc[7] = 8;
  abc[8] = 9;
  abc[9] = 10;

  (a.b->z.c->d->u -= abc[a.b->y++]++, a.b->z.c->d->v += abc[a.b->y--]--, a.b->z.c->z += abc[a.b->y++]++);

  puts("a.b->z.c->d->v = ");
  char numbuf[32];
  itoa_no_div((uint64_t)a.b->z.c->d->v, numbuf);
  puts(numbuf);

  puts("a.b->z.c->d->u = ");
  itoa_no_div((uint64_t)a.b->z.c->d->u, numbuf);
  puts(numbuf);

  return 0;
}