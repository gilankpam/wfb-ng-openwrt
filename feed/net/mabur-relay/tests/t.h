#ifndef T_H
#define T_H
#include <stdio.h>
extern int t_failures;
#define CHECK(c) do { if (!(c)) { t_failures++; \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); \
  if (_a != _b) { t_failures++; fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n", \
  __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)
void t_wire(const char *fixtures_dir);
#endif
