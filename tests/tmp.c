#include <stdint.h>
#include <stdio.h>
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

struct test {
  struct test *next;
  uint64_t a;
};

int main(void) {
  struct test t;
  t.next = NULL;
  t.a = 4;

  struct test t2;
  t2.next = &t;
  t2.a = 3;

  struct test t3;
  t3.next = &t2;
  t3.a = 2;

  t3.next->next->a = 7;

  char buf[32];
  itoa_no_div((uint64_t)t3.next->next->a, buf);
  puts("Value: ");
  puts(buf);

  return 0;
}
