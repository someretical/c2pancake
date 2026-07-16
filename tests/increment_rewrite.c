// ++ / -- edge case test suite
// Each case is isolated in its own block.
// Dummy types/functions are provided so the file parses.

#include <stddef.h>

struct Child {
  int value;
};

struct Obj {
  int field;
  int arr[16];
  struct Child child;
};

int foo() { return 123; }

int foo1(int a) { return a; }

int foo2(int a, int b) { return a + b; }

int foo3(int a, int b, int c) { return a + b + c; }

int bar(int x) { return x; }

int baz(int x) { return x; }

int qux(int x) { return x; }

int main() {
  // basic
  {
    int x = 5;
    int a = ++x;
    (void)a;
    // output is broken
    // int x = 5;
    // x = x + 1
    // (void)a;
  }

  {
    int x = 5;
    int a = x++;
    (void)a;
  }

  {
    int x = 5;
    int a = --x;
    (void)a;
  }

  {
    int x = 5;
    int a = x--;
    (void)a;
  }

  // multiple operators
  {
    int x = 5;
    int a = ++x + ++x;
    (void)a;
  }

  {
    int x = 5;
    int a = x++ + x++;
    (void)a;
  }

  {
    int x = 5;
    int a = x++ + ++x;
    (void)a;
  }

  {
    int x = 5;
    int a = --x + x--;
    (void)a;
  }

  // nesting
  {
    int x = 5;
    int y = 10;
    int z = x++ + y++;
    (void)z;
  }

  {
    int x = 5;
    int y = ++x * x++;
    (void)y;
  }

  {
    int x = 5;
    int y = (x++) + (x++);
    (void)y;
  }

  // assignment
  {
    int x = 5;
    x = x++;
  }

  {
    int x = 5;
    x = ++x;
    // produces
    // {
    //   int x = 5;
    //   x = x + 1;
    //   x = x;
    // }
    // last line is redundant
  }

  {
    int x = 5;
    x += x++;
  }

  {
    int x = 5;
    x += ++x;
  }

  // function args
  {
    int x = 5;
    foo2(x++, x++);
  }

  {
    int x = 5;
    foo2(++x, x);
  }

  {
    int x = 5;
    foo2(x, ++x);
  }

  {
    int x = 5;
    foo3(x++, ++x, x--);
  }

  // array indexing
  {
    int arr[10] = {};
    int i = 0;
    arr[i++] = 42;
  }

  {
    int arr[10] = {};
    int i = 0;
    arr[++i] = 42;
  }

  {
    // this one is UNDEFINED behaviour in C!!!
    int arr[10] = {};
    int i = 0;
    arr[i++] = arr[i++];
  }

  {
    // so is this one!
    int arr[10] = {};
    int i = 1;
    arr[i++] = arr[++i];
  }

  // struct field access
  {
    struct Obj obj;
    obj.field++;
  }

  {
    struct Obj obj;
    ++obj.field;
  }

  {
    struct Obj obj;
    int i = 0;
    obj.arr[i++]++;
  }

  {
    struct Obj obj;
    ++obj.child.value;
  }

  {
    int arr[10] = {};
    int i = 0;
    struct Obj obj;
    arr[obj.arr[i++]]++;
  }

  // pointers
  {
    int value = 10;
    int *ptr = &value;
    (*ptr)++;
  }

  {
    int value = 10;
    int *ptr = &value;
    ++(*ptr);
  }

  {
    int values[4] = {};
    int *ptr = values;
    // pointer arithmetic!
    ptr++;
  }

  {
    int values[4] = {1, 2, 3, 4};
    int *ptr = values;
    int x = *ptr++;
    (void)x;
  }

  {
    int values[4] = {1, 2, 3, 4};
    int *ptr = values;
    int arr[4] = {};
    // currently broken!
    arr[(*ptr)++]++;
  }

  // chaining
  {
    int x = 5;
    - - --x; // tokenization stress
  }

  // conditionals
  {
    int x = 5;
    bool cond = true;
    int y = cond ? x++ : --x;
    (void)y;
  }

  {
    int x = 5;
    int y = 7;
    bool cond = false;
    cond ? ++x : ++y;
  }

  // loops
  {
    int n = 5;
    for (int i = 0; i < n; i++) {
    }
  }

  {
    int n = 5;
    for (int i = 0; i < n; ++i) {
    }
  }

  {
    int i = 5;
    while (i--) {
    }
  }

  {
    int i = 5;
    while (--i) {
    }
  }

  // returns
  {
    int x = 5;
    int r = x++;
    (void)r;
  }

  {
    int x = 5;
    int r = ++x;
    (void)r;
  }

  // short circuiting
  {
    int x = 1;
    int y = 1;
    if (x++ && y++) {
    }
  }

  {
    int x = 0;
    int y = 1;
    if (++x || ++y) {
    }
  }

  {
    int x = 1;
    bool b = x++ && x++;
    (void)b;
  }

  // these should generate warnings about sequence points
  {
    int a = 1;
    int b = 2;
    int r = a++ + b; // (a++) + b
    (void)r;
  }

  {
    int a = 5;
    int b = 2;
    int r = a-- - b; // (a--) - b
    (void)r;
  }

  // side effects
  {
    int arr[20] = {};
    int i = 1;
    arr[i++] = arr[++i] + arr[i++];
  }

  {
    int x = 5;
    foo3(bar(x++), baz(++x), qux(x--));
  }

  {
    int x = 5;
    x = x++ + ++x + x-- + --x;
  }

  {
    int arr[20] = {};
    int i = 1;
    arr[arr[i++]++]++;
  }

  return 0;
}