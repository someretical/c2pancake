int main(void) {
  int a = 0;
  int b = 1;
  while (a++ || b--) {
    b++;
  }
  return 0;
}