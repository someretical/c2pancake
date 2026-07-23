// # 0 "tests/cast.c"
// # 0 "<built-in>"
// # 0 "<command-line>"
// # 1 "/usr/include/stdc-predef.h" 1 3 4
// # 0 "<command-line>" 2
// # 1 "tests/cast.c"
// # 11 "tests/cast.c"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

int main() {
  if ((131585) != ((int)8590066177)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 12, "131585", "(int)8590066177");
    printf("  %lld != %lld\n", (long long)(131585), (long long)((int)8590066177));
    return 1;
  };
  if ((513) != ((short)8590066177)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 13, "513", "(short)8590066177");
    printf("  %lld != %lld\n", (long long)(513), (long long)((short)8590066177));
    return 1;
  };
  if ((1) != ((char)8590066177)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 14, "1", "(char)8590066177");
    printf("  %lld != %lld\n", (long long)(1), (long long)((char)8590066177));
    return 1;
  };
  if ((1) != ((long)1)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 15, "1", "(long)1");
    printf("  %lld != %lld\n", (long long)(1), (long long)((long)1));
    return 1;
  };
  if ((0) != ((long)&*(int *)0)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 16, "0", "(long)&*(int *)0");
    printf("  %lld != %lld\n", (long long)(0), (long long)((long)&*(int *)0));
    return 1;
  };
  if ((513) != (({
        int x = 512;
        *(char *)&x = 1;
        x;
      }))) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 17, "513", "({ int x = 512; *(char *)&x = 1; x; })");
    printf("  %lld != %lld\n", (long long)(513), (long long)(({
             int x = 512;
             *(char *)&x = 1;
             x;
           })));
    return 1;
  }

  ;
  if ((5) != (({
        int x = 5;
        long y = (long)&x;
        *(int *)y;
      }))) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 22, "5",
           "({ int x = 5; long y = (long)&x; *(int *)y; })");
    printf("  %lld != %lld\n", (long long)(5), (long long)(({
             int x = 5;
             long y = (long)&x;
             *(int *)y;
           })));
    return 1;
  }

  ;

  (void)1;

  if ((-1) != ((char)255)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 30, "-1", "(char)255");
    printf("  %lld != %lld\n", (long long)(-1), (long long)((char)255));
    return 1;
  };
  if ((-1) != ((signed char)255)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 31, "-1", "(signed char)255");
    printf("  %lld != %lld\n", (long long)(-1), (long long)((signed char)255));
    return 1;
  };
  if ((255) != ((unsigned char)255)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 32, "255", "(unsigned char)255");
    printf("  %lld != %lld\n", (long long)(255), (long long)((unsigned char)255));
    return 1;
  };
  if ((-1) != ((short)65535)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 33, "-1", "(short)65535");
    printf("  %lld != %lld\n", (long long)(-1), (long long)((short)65535));
    return 1;
  };
  if ((65535) != ((unsigned short)65535)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 34, "65535", "(unsigned short)65535");
    printf("  %lld != %lld\n", (long long)(65535), (long long)((unsigned short)65535));
    return 1;
  };
  if ((-1) != ((int)0xffffffff)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 35, "-1", "(int)0xffffffff");
    printf("  %lld != %lld\n", (long long)(-1), (long long)((int)0xffffffff));
    return 1;
  };
  if ((0xffffffff) != ((unsigned)0xffffffff)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 36, "0xffffffff", "(unsigned)0xffffffff");
    printf("  %lld != %lld\n", (long long)(0xffffffff), (long long)((unsigned)0xffffffff));
    return 1;
  };

  if ((1) != (-1 < 1)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 38, "1", "-1 < 1");
    printf("  %lld != %lld\n", (long long)(1), (long long)(-1 < 1));
    return 1;
  };
  if ((0) != (-1 < (unsigned)1)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 39, "0", "-1 < (unsigned)1");
    printf("  %lld != %lld\n", (long long)(0), (long long)(-1 < (unsigned)1));
    return 1;
  };
  if ((254) != ((char)127 + (char)127)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 40, "254", "(char)127 + (char)127");
    printf("  %lld != %lld\n", (long long)(254), (long long)((char)127 + (char)127));
    return 1;
  };
  if ((65534) != ((short)32767 + (short)32767)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 41, "65534", "(short)32767 + (short)32767");
    printf("  %lld != %lld\n", (long long)(65534), (long long)((short)32767 + (short)32767));
    return 1;
  };
  if ((-1) != (-1 >> 1)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 42, "-1", "-1 >> 1");
    printf("  %lld != %lld\n", (long long)(-1), (long long)(-1 >> 1));
    return 1;
  };
  if ((-1) != ((unsigned long)-1)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 43, "-1", "(unsigned long)-1");
    printf("  %lld != %lld\n", (long long)(-1), (long long)((unsigned long)-1));
    return 1;
  };
  if ((2147483647) != (((unsigned)-1) >> 1)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 44, "2147483647", "((unsigned)-1) >> 1");
    printf("  %lld != %lld\n", (long long)(2147483647), (long long)(((unsigned)-1) >> 1));
    return 1;
  };
  if ((-50) != ((-100) / 2)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 45, "-50", "(-100) / 2");
    printf("  %lld != %lld\n", (long long)(-50), (long long)((-100) / 2));
    return 1;
  };
  if ((2147483598) != (((unsigned)-100) / 2)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 46, "2147483598", "((unsigned)-100) / 2");
    printf("  %lld != %lld\n", (long long)(2147483598), (long long)(((unsigned)-100) / 2));
    return 1;
  };
  if ((9223372036854775758) != (((unsigned long)-100) / 2)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 47, "9223372036854775758",
           "((unsigned long)-100) / 2");
    printf("  %lld != %lld\n", (long long)(9223372036854775758), (long long)(((unsigned long)-100) / 2));
    return 1;
  };
  if ((0) != (((long)-1) / (unsigned)100)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 48, "0", "((long)-1) / (unsigned)100");
    printf("  %lld != %lld\n", (long long)(0), (long long)(((long)-1) / (unsigned)100));
    return 1;
  };
  if ((-2) != ((-100) % 7)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 49, "-2", "(-100) % 7");
    printf("  %lld != %lld\n", (long long)(-2), (long long)((-100) % 7));
    return 1;
  };
  if ((2) != (((unsigned)-100) % 7)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 50, "2", "((unsigned)-100) % 7");
    printf("  %lld != %lld\n", (long long)(2), (long long)(((unsigned)-100) % 7));
    return 1;
  };
  if ((6) != (((unsigned long)-100) % 9)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 51, "6", "((unsigned long)-100) % 9");
    printf("  %lld != %lld\n", (long long)(6), (long long)(((unsigned long)-100) % 9));
    return 1;
  };

  if ((65535) != ((int)(unsigned short)65535)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 53, "65535", "(int)(unsigned short)65535");
    printf("  %lld != %lld\n", (long long)(65535), (long long)((int)(unsigned short)65535));
    return 1;
  };
  if ((65535) != (({
        unsigned short x = 65535;
        x;
      }))) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 54, "65535", "({ unsigned short x = 65535; x; })");
    printf("  %lld != %lld\n", (long long)(65535), (long long)(({
             unsigned short x = 65535;
             x;
           })));
    return 1;
  }

  ;
  if ((65535) != (({
        unsigned short x = 65535;
        (int)x;
      }))) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 58, "65535",
           "({ unsigned short x = 65535; (int)x; })");
    printf("  %lld != %lld\n", (long long)(65535), (long long)(({
             unsigned short x = 65535;
             (int)x;
           })));
    return 1;
  }

  ;

  if ((-1) != (({
        typedef short T;
        T x = 65535;
        (int)x;
      }))) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 63, "-1",
           "({ typedef short T; T x = 65535; (int)x; })");
    printf("  %lld != %lld\n", (long long)(-1), (long long)(({
             typedef short T;
             T x = 65535;
             (int)x;
           })));
    return 1;
  }

  ;
  if ((65535) != (({
        typedef unsigned short T;
        T x = 65535;
        (int)x;
      }))) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 68, "65535",
           "({ typedef unsigned short T; T x = 65535; (int)x; })");
    printf("  %lld != %lld\n", (long long)(65535), (long long)(({
             typedef unsigned short T;
             T x = 65535;
             (int)x;
           })));
    return 1;
  }

  ;

  if ((0) != ((_Bool)0.0)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 74, "0", "(_Bool)0.0");
    printf("  %lld != %lld\n", (long long)(0), (long long)((_Bool)0.0));
    return 1;
  };
  if ((1) != ((_Bool)0.1)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 75, "1", "(_Bool)0.1");
    printf("  %lld != %lld\n", (long long)(1), (long long)((_Bool)0.1));
    return 1;
  };
  if ((3) != ((char)3.0)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 76, "3", "(char)3.0");
    printf("  %lld != %lld\n", (long long)(3), (long long)((char)3.0));
    return 1;
  };
  if ((1000) != ((short)1000.3)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 77, "1000", "(short)1000.3");
    printf("  %lld != %lld\n", (long long)(1000), (long long)((short)1000.3));
    return 1;
  };
  if ((3) != ((int)3.99)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 78, "3", "(int)3.99");
    printf("  %lld != %lld\n", (long long)(3), (long long)((int)3.99));
    return 1;
  };
  if ((2000000000000000) != ((long)2e15)) {
    printf("ASSERTION FAILED: %s:%d: %s != %s\n", "tests/cast.c", 79, "2000000000000000", "(long)2e15");
    printf("  %lld != %lld\n", (long long)(2000000000000000), (long long)((long)2e15));
    return 1;
  };

  printf("OK\n");
  return 0;
}
