#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct d {
  int v;
  int u;
};

struct c {
  int z;
  struct d *d;
};

struct z {
  int w;
  struct c *c;
};

struct b {
  int y;
  struct z z;
};

struct a {
  int x;
  struct b *b;
};

int main(void) {
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

  int abc[10];
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

  assert(a.b->z.c->d->v == 16);
  assert(a.b->z.c->d->u == 42);

  return 0;
}