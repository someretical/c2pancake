#pragma once // very important!

struct ffi_type {
  int a;
  int b;
  struct ffi_type *elements[4];
};

// does something but the def is not accessible...
void ffi_func_1(struct ffi_type arg1, int arg2);

// does something but the def is not accessible...
struct ffi_type ffi_func_2(struct ffi_type arg1, int arg2);
