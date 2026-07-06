int main(void) {
  int a = 0;
  int b = 1;
  int c = 2;
  while ((c++ && (c < 10 || a > 2)) && (a++ || b--)) {
    b++;
    c = (a + b > 10) ? (a - b) : (a + b);
  }

  int d[10];
  d[b++] += c;

  return 0;
}