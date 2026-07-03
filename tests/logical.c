int main(void) {
  int a = 0;
  int b = 1;
  int c = 2;
  while ((c++ && (c < 10 || a > 2)) && (a++ || b--)) {
    b++;
  }
  return 0;
}