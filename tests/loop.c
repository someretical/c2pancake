#include <stdio.h>

int main(void) {
  for (int i = 0; i < 10; i++) {
    if (i % 2 == 0)
      continue;
    printf("%d\n", i);
  }

  for (int i = 0; i < 10; i++) {
    if (i % 2 == 0) {
      for (int j = 0; j < 5; j++) {
        if (j == 3) {
          continue;
        }
        printf("%d %d\n", i, j);
      }
    }
  }

  for (;;) {
  }

  do {
    break;
  } while (1);

  int i = 2;
  do {
    if (i > 3) {
      break;
    }
  } while (i < 5);

  do {
    if (i > 3) {
      continue;
    } else if (i == 3) {
      break;
    }
    i++;
  } while (i < 5);

  do {
  } while (0);

  return 0;
}
