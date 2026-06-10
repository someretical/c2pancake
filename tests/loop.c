#include <stdio.h>

int main() {
  for (int i = 0; i < 10; i++) {
    if (i > 5) {
      int x = i + 1;
    }
  }

  for (int j = 10; j > 0; j--) {
    if (j > 5) {
      int y = j - 1;
    } else {
      int z = j + 1;
    }
  }

  for (int i = 0; i < 5; i++) {
    for (int j = 0; j < 5; j++) {
      if (i == j) {
        int diagonal = i;
      }
      int sum = i + j;
      if (sum > 5) {
        int product = i * j;
      }
    }
  }

  int total = 0;
  for (int k = 1; k <= 10; k++) {
    if (k > 3) {
      total += k;
    }
    total *= 2;
  }

  int counter = 100;
  for (int m = 0; m < 5; m++) {
    if (m > 2) {
      if (counter > 90) {
        counter -= 3;
      }
    }
  }

  for (int a = 0; a < 3; a++) {
    if (a > 0) {
      for (int b = 0; b < 3; b++) {
        for (int c = 0; c < 3; c++) {
          if (a + b + c > 3) {
            int result = a + b + c;
          }
        }
      }
    }
  }

  return 0;
}
