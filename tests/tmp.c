struct test {
  int a;
  int b;
  int c;
};

struct test2 {
  int a;
  int b;
  int c;
};

struct test2 func1(int a, int b, int c, struct test2 t) {
  struct test2 t2 = {0};
  t2.a = a + t.a;
  t2.b = b + t.b;
  t2.c = c + t.c;
  return t2;
}

void func2(void) {}

int main(void) {}
