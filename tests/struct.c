struct Point {
  int x;
  int y;
};

struct Vec3 {
  int a;
  int b;
  int c;
};

int add_point(struct Point p) { return p.x + p.y; }

int main() {
  struct Point p = {10, 20};
  int sum = add_point(p);

  int px = p.x;
  int py = p.y;

  p.x = 99;

  struct Vec3 v = {1, 2, 3};
  int total = v.a + v.b + v.c;

  v.b = 42;

  struct Point q = {v.a, v.c};

  return 0;
}
