#include <stdio.h>
#include "t.h"
int t_failures = 0;
int main(int argc, char **argv) {
  const char *fx = argc > 1 ? argv[1] : "../tests/fixtures";
  t_wire(fx);
  t_filter(fx);
  t_rtap(fx);
  t_ws(fx);
  t_tune(fx);
  t_txrt(fx);
  if (t_failures) { fprintf(stderr, "%d FAILURE(S)\n", t_failures); return 1; }
  printf("ALL PASS\n");
  return 0;
}
