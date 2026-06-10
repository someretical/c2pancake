struct Point {
  int x;
  int y;
};

enum Color { RED, GREEN, BLUE };
enum Priority { LOW = 10, MED = 20, HIGH = 30 };

int arr[5] = {10, 20, 30, 40, 50};
int buf[3];

int add(int a, int b) { return a + b; }

int sum_array(int n) {
  int total = 0;
  int i = 0;
  while (i < n) {
    total = total + arr[i];
    i = i + 1;
  }
  return total;
}

int main() {
  int errors = 0;

  int sum = add(3, 4);
  if (sum != 7) {
    errors = errors + 1;
  }

  struct Point p = {10, 20};
  if (p.x != 10) {
    errors = errors + 1;
  }
  if (p.y != 20) {
    errors = errors + 1;
  }
  p.x = 99;
  if (p.x != 99) {
    errors = errors + 1;
  }
  if (p.y != 20) {
    errors = errors + 1;
  }
  int psum = add(p.x, p.y);
  if (psum != 119) {
    errors = errors + 1;
  }

  int x = 3;
  int result = 0;
  switch (x) {
  case 1:
    result = 10;
    break;
  case 2:
    result = 20;
    break;
  case 3:
    result = 30;
    break;
  default:
    result = 99;
    break;
  }
  if (result != 30) {
    errors = errors + 1;
  }

  int cat = 0;
  switch (x) {
  case 1:
  case 2:
  case 3:
    cat = 1;
    break;
  case 4:
  case 5:
    cat = 2;
    break;
  default:
    cat = 0;
    break;
  }
  if (cat != 1) {
    errors = errors + 1;
  }

  int count = 0;
  int total = 0;
  do {
    total = total + count;
    count = count + 1;
  } while (count < 5);
  if (total != 10) {
    errors = errors + 1;
  }
  if (count != 5) {
    errors = errors + 1;
  }

  int fsum = 0;
  for (int i = 1; i <= 5; i++) {
    fsum = fsum + i;
  }
  if (fsum != 15) {
    errors = errors + 1;
  }

  int v = 10;
  v += 5;
  v -= 3;
  v *= 2;
  if (v != 24) {
    errors = errors + 1;
  }

  int grade = 0;
  int score = 85;
  if (score >= 90) {
    grade = 4;
  } else {
    if (score >= 80) {
      grade = 3;
    } else {
      grade = 2;
    }
  }
  if (grade != 3) {
    errors = errors + 1;
  }

  int bi = 0;
  while (bi < 100) {
    if (bi == 7) {
      break;
    }
    bi = bi + 1;
  }
  if (bi != 7) {
    errors = errors + 1;
  }

  int csum = 0;
  int ci = 0;
  while (ci < 6) {
    ci = ci + 1;
    if (ci == 3) {
      continue;
    }
    csum = csum + ci;
  }
  if (csum != 18) {
    errors = errors + 1;
  }

  int ta = 5;
  int tb = (ta > 3) ? 10 : 20;
  if (tb != 10) {
    errors = errors + 1;
  }
  int tc = 0;
  tc = (ta == 5) ? 100 : 200;
  if (tc != 100) {
    errors = errors + 1;
  }

  int color = GREEN;
  if (color != 1) {
    errors = errors + 1;
  }
  int pri = HIGH;
  if (pri != 30) {
    errors = errors + 1;
  }

  if (arr[0] != 10) {
    errors = errors + 1;
  }
  if (arr[4] != 50) {
    errors = errors + 1;
  }
  int asum = sum_array(5);
  if (asum != 150) {
    errors = errors + 1;
  }
  arr[0] = 99;
  if (arr[0] != 99) {
    errors = errors + 1;
  }
  if (arr[1] != 20) {
    errors = errors + 1;
  }

  buf[0] = 100;
  buf[1] = 200;
  buf[2] = 300;
  if (buf[0] != 100) {
    errors = errors + 1;
  }
  if (arr[1] != 20) {
    errors = errors + 1;
  }

  int local[4] = {7, 8, 9, 10};
  if (local[0] != 7) {
    errors = errors + 1;
  }
  if (local[3] != 10) {
    errors = errors + 1;
  }
  local[0] = 77;
  if (local[0] != 77) {
    errors = errors + 1;
  }
  if (buf[0] != 100) {
    errors = errors + 1;
  }

  int lsum = 0;
  for (int i = 0; i < 4; i++) {
    lsum = lsum + local[i];
  }
  if (lsum != 104) {
    errors = errors + 1;
  }

  if (errors == 0) {
    // @ffi: report_pass
    errors;
  } else {
    // @ffi: report_fail
    errors;
  }

  return 0;
}
