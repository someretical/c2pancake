struct Child {
  int value;
};

struct Obj {
  int field;
  int arr[16];
  struct Child child;
};

int main(void) {
  int arr[10] = {};
  int i = 0;
  struct Obj obj{};
  arr[obj.arr[i++]]++;
  return 0;
}