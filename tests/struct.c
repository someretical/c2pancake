int main(void) {
  struct LocalRect {
    short width;
    short height;
  } r = {100, 200};
  (void)r;

  for (struct S { int x; } a = {5}, *p = &a, c[10] = {{0}}; a.x++; a.x++) {
    (void)p;
    (void)c;
  }

  return 0;
}