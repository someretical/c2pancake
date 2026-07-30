struct S {
  char a[5];
  int b;
};

int main(void) {
  struct S s = {"abc", 42};
  (void)s;
  char b[] = "hello";
  (void)b;
  const char *c = "world";
  (void)c;
  return 0;
}