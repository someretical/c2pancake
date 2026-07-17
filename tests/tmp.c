#include <stdio.h>

// #define MAX(a, b) \
//   ({ \
//     __typeof__(a) _a = (a); \
//     __typeof__(b) _b = (b); \
//     _a > _b ? _a : _b; \
//   })

int foo(int x) {
  return ({
    int y = x * 2;

    if (y > 10) {
      y -= 3;
    } else {
      y += 5;
    }

    int z = ({
      int t = y + 1;
      t *t;
    });

    ({
      __typeof__(y) _y = (y);
      __typeof__(z) _z = (z);
      _y > _z ? _y : _z;
    }) + ({
      int w = z / 2;
      w;
    });
  });
}

int main(void) {
  printf("%d\n", foo(4));
  printf("%d\n", foo(10));
}