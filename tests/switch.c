int main(void) {
  int i = 2;
  switch (i) {
    [[fallthrough]];
  case -1:
    [[fallthrough]];
  case 0:
  case 1:
    break;
  case 2:
    i = 3;
    i = 4;
    break;
  case 3: {
    i = 5;
    break;
  }
  case 4:
    i = 6;
    i = 7;
  case 5:
  case 7:
    i = 8;
    i = 9;
    return 0; // also account for break and continue here...

  default:
    i = 10;

  case 10:
    i = 11;
  }
}

int main2(void) {
  int i = 2;
  switch (i) {
  case 1: {
    break;
  }
  case 2: {
    i = 3;
    i = 4;
    break;
  }
  case 3: {
    i = 5;
    break;
  }
  case 4: {
    i = 6;
    i = 7;
    if (i == 7) {
      i = 8;
      i = 9;
    }
    break;
  }
  case 5: {
    i = 8;
    i = 9;
    return 0;
  }
  case 7: {
    i = 8;
    i = 9;
    return 0;
  }

  default: {
    i = 10;
    if (i == 10) {
      i = 11;
    }
    break;
  }

  case 10: {
    i = 11;
    break;
  }
  }
}