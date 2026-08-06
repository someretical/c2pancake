#include <stdio.h>
#include <string.h>

/* External symbol used by the extern-local test below */
int g_external_counter = 0;

/*  Case 1: plain auto array in a function  */
int sum_array(void) {
  int data[8] = {1, 2, 3, 4, 5, 6, 7, 8}; /* hoisted: auto array */
  int total = 0;
  for (int i = 0; i < 8; i++)
    total += data[i];
  return total;
}

/*  Case 2: auto local whose address is taken  */
void inc_via_ptr(void) {
  int counter = 0; /* hoisted: address taken below */
  int *p = &counter;
  (*p)++;
  // printf("counter = %d\n", counter);
}

/*  Case 3: mix – auto array + auto address-taken scalar + plain local
 */
void mixed(void) {
  char msg[64];   /* hoisted: auto array               */
  int value = 42; /* hoisted: address taken            */
  int plain = 7;  /* NOT hoisted: plain auto local     */
  int *vp = &value;

  // snprintf(msg, sizeof(msg), "value=%d plain=%d", value, plain);
  puts(msg);
  (*vp) += plain;
  // printf("after: %d\n", value);
}

/*  Case 4: static local array */
void static_array(void) {
  /* classic "call counter" pattern – already static, now hoisted globally */
  static int call_log[16];   /* hoisted: static array */
  static int call_count = 0; /* hoisted: static, address taken below */
  int *cp = &call_count;
  call_log[*cp % 16] = *cp;
  (*cp)++;
  // printf("call #%d\n", call_count);
}

/*  Case 5: static local with constant initialiser  */
void static_const_init(void) {
  /* The initialiser is a compile-time constant → kept in the global decl */
  static int threshold[4] = {10, 20, 30, 40}; /* hoisted: static array, const init */
  static int base = 100;                      /* hoisted: static, address taken, const init */
  int *bp = &base;
  for (int i = 0; i < 4; i++)
    ;
  // printf("%d ", threshold[i] + *bp);
  puts("");
}

/*  Case 6: extern local re-declaration */
void use_extern(void) {
  /* This re-declares the file-scope g_external_counter inside the function.
     The local extern decl is removed; uses are renamed to the bare symbol. */
  extern int g_external_counter; /* hoisted: extern redecl → removed */
  g_external_counter++;
  // printf("extern counter = %d\n", g_external_counter);
}

/*  Case 7: nested scopes with a static local */
void nested(int n) {
  for (int i = 0; i < n; i++) {
    int buf[16];           /* hoisted: auto array inside loop   */
    static int visits = 0; /* hoisted: static scalar, address taken */
    int *vp = &visits;
    buf[0] = i;
    (*vp)++;
    // printf("iter %d visits %d buf[0]=%d\n", i, visits, buf[0]);
  }
}

/*  Case 8: multi-dimensional array */
void matrix(void) {
  long mat[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  // printf("%ld\n", mat[1][1]);
}

/*  Case 9: address taken through cast  */
void byte_alias(void) {
  unsigned int word = 0xDEADBEEF; /* hoisted: address taken via cast */
  unsigned char *bp = (unsigned char *)&word;
  // printf("%02x\n", bp[0]);
}

/* Case 10: multiple init on one line */
void multiple_init(void) {
  int a[3][4], b[5], c = 15;
  // printf("a = %p, b = %p, c = %d\n", (void *)a, (void *)b, c);
}

void struct_test(void) {
  struct S {
    int arr[4]; /* hoisted: auto array in struct */
  };
  struct S s = {{1, 2, 3, 4}};
  // printf("%d\n", s.arr[2]);
}

void struct_pointer_test(void) {
  struct S {
    int field; /* hoisted: address taken via pointer in struct */
  };
  struct S s = {42};
  struct S *sp = &s;
  // printf("%d\n", sp->field);
}

void struct_unused_arr_test(void) {
  struct S {
    int arr[4]; /* not hoisted: since it is never used */
    int field2; /* not hoisted: address not taken, not an array */
  };
  struct S s;
  s.field2 = 42; /* only field2 is used, so arr is not hoisted */
  // printf("%d\n", s.field2);
}

void struct_unused_arr_ptr_test(void) {
  struct S {
    int arr[4]; /* not hoisted: since it is never used */
    int field2; /* not hoisted: address not taken, not an array */
  };
  struct S s;
  struct S *sp = &s;
  sp->field2 = 42; /* S has to be hoisted now... */
  // printf("%d\n", sp->field2);
}

void array_of_structs(void) {
  struct S {
    int x;
    int y;
  };
  struct S points[3] = {{1, 2}, {3, 4}, {5, 6}}; /* hoisted: auto array of structs */
  for (int i = 0; i < 3; i++)
    ;
  // printf("point %d: (%d, %d)\n", i, points[i].x, points[i].y);
}

int main(void) {
  // printf("sum  = %d\n", sum_array());
  inc_via_ptr();
  mixed();
  static_array();
  static_array(); /* second call – tests that call_count persists */
  static_const_init();
  use_extern();
  nested(3);
  matrix();
  byte_alias();
  multiple_init();
  return 0;
}
