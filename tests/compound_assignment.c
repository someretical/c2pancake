int main(void) {
  int arr[10] = {0};
  int i = 0;
  arr[i += 3] += 2;
  return 0;
}

int sum_array(void) {
  int data[8] = {1, 2, 3, 4, 5, 6, 7, 8}; /* hoisted: auto array */
  int total = 0;
  for (int i = 0; i < 8; i++)
    total += data[i];
  return total;
}