// struct test {
//   int a;
//   int b;
//   int c;
// };

// struct test2 {
//   int a;
//   int b;
//   int c;
// };

// struct test2 func1(int a, int b, int c, struct test2 t) {
//   struct test2 t2 = {0};
//   t2.a = a + t.a;
//   t2.b = b + t.b;
//   t2.c = c + t.c;
//   return t2;
// }

// void func2(void) {}

// int main(void) {}

struct arg {
  int a;
  int b;
  int c;
};

struct arg main2(struct arg args) {
  args.a += 1;
  return args;
}

struct arg main3(struct arg args1, struct arg args2) {
  struct arg result = {args1.a + args2.a, 0, 0};
  return result;
}

int main(void) {
  struct arg arg1 = {5, 0, 0};
  struct arg arg2 = {7, 0, 0};
  struct arg result = main3(main2(arg1), main2(arg2));
  return result.a;
}