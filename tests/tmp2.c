int main(void) {
  struct A {
    unsigned long a[3];
  };

  struct A a = {.a = {1, 2, 3}};

  struct A a2;
  a2 = a;

  struct A *p = ({
    struct A *src = &a;
    &a2;
  });
}