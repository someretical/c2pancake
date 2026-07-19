int main(void) {
  union U {
    struct {
      struct {
        int a;
        int b;
      };
    };
    int c;
  } u;

  union U up = {.a = 1, .b = 2};

  int arr[10] = {0};

  struct A {
    int x;
    int y;
    struct {
      int z;
      int w;
    };
  };

  struct A a = {.y = 2};

  float mat[3][3][3] = {{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
                        {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
                        {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};

  struct Cell {
    int values[2][3];
  };

  struct Layer {
    struct Cell cells[2];
  };

  struct World {
    struct Layer layers[2];
    const char *name;
  };

  struct World world = {
      .layers = {{.cells = {{.values = {{1, 2, 3}, {4, 5, 6}}}, {.values = {{7, 8, 9}, {10, 11, 12}}}}},
                 {.cells = {{.values = {{13, 14, 15}, {16, 17, 18}}}, {.values = {{19, 20, 21}, {22, 23, 24}}}}}},
      .name = "demo"};

  union Value {
    int i;
    float f;
  };

  struct Entry {
    const char *name;
    union Value value;
  };

  struct Entry table[] = {{"one", {.i = 1}}, {"pi", {.f = 3.14159f}}, {"forty", {.i = 40}}};

  struct Inner {
    int a;
    int b;
  };

  struct Outer {
    struct Inner items[2];
    int count;
  };

  struct Outer obj = {.items = {[0] = {.a = 1, .b = 2}, [1] = {.b = 4, .a = 3}}, .count = 2};

  struct Vec3 {
    float x, y, z;
  };

  struct Mesh {
    int ids[4];
    struct Vec3 vertices[2];
  };

  struct Mesh mesh = {{1, 2, 3, 4}, {{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}}};

  return 0;
}