#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
  int started = 0;

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

int main(void) {
  const char *a = "hello world\n";
  write(1, a, strlen(a));

  uint64_t b[5][4][3] = {{{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {10, 11, 12}},
                         {{13, 14, 15}, {16, 17, 18}, {19, 20, 21}, {22, 23, 24}},
                         {{25, 26, 27}, {28, 29, 30}, {31, 32, 33}, {34, 35, 36}},
                         {{37, 38, 39}, {40, 41, 42}, {43, 44, 45}, {46, 47, 48}},
                         {{49, 50, 51}, {52, 53, 54}, {55, 56, 57}, {58, 59, 60}}};

  char buf[3] = {0};
  for (uint64_t i = 0; i < 5; i++) {
    for (uint64_t j = 0; j < 4; j++) {
      for (uint64_t k = 0; k < 3; k++) {
        itoa_no_div(b[i][j][k], buf);
        write(1, buf, 3);
        write(1, " ", 1);
      }
      write(1, "\n", 1);
    }
    write(1, "\n", 1);
  }

  return 0;
}
