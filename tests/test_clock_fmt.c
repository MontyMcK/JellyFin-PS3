// Host test for source/ui/render/clock_fmt.h.   make -f Makefile.host test_clock_fmt
#include <stdio.h>
#include <string.h>
#include "clock_fmt.h"
static int n = 0, bad = 0;
#define CHECK(c) do { n++; if (!(c)) { bad++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static void f(long long s, int off, int df, int tf, const char *wt, const char *wd) {
    char t[16], d[16];
    clock_format(s, off, df, tf, t, sizeof t, d, sizeof d);
    if (strcmp(t, wt) || strcmp(d, wd)) printf("got '%s' '%s', want '%s' '%s'\n", t, d, wt, wd);
    CHECK(!strcmp(t, wt) && !strcmp(d, wd));
}
int main(void) {
    const long long base = 1791122700LL;   // 2026-10-04 14:05:00 UTC
    f(base, 0, CLK_DATE_MDY, CLK_TIME_24H, "14:05", "10/4");
    f(base, 0, CLK_DATE_MDY, CLK_TIME_12H, "2:05 PM", "10/4");
    f(base, 0, CLK_DATE_DMY, CLK_TIME_24H, "14:05", "4/10");
    f(base, 3600, CLK_DATE_DMY, CLK_TIME_24H, "15:05", "4/10");          // summer time
    f(base, -5 * 3600, CLK_DATE_MDY, CLK_TIME_12H, "9:05 AM", "10/4");
    f(base + 10 * 3600, 0, CLK_DATE_MDY, CLK_TIME_12H, "12:05 AM", "10/5"); // midnight is 12, date rolls
    f(base - 2 * 3600 - 5 * 60, 0, CLK_DATE_YMD, CLK_TIME_12H, "12:00 PM", "10/4");
    printf("%d checks, %d failed\n", n, bad);
    return bad != 0;
}
