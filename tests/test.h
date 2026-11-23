/* Nora - minimal test harness. MIT License. */
#ifndef NORA_TEST_H
#define NORA_TEST_H
#include <stdio.h>
#include <math.h>
extern int g_test_failures;
extern int g_test_checks;
#define CHECK(c) do { g_test_checks++; if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_test_failures++; } } while (0)
#define CHECK_NEAR(a, b, eps) do { double a_ = (double)(a), b_ = (double)(b); g_test_checks++; \
    if (!(fabs(a_ - b_) <= (eps))) { printf("  FAIL %s:%d: %s = %g, expected %g (+-%g)\n", __FILE__, __LINE__, #a, a_, b_, (double)(eps)); g_test_failures++; } } while (0)
void test_spectrum(void);
void test_bsdf(void);
void test_bvh(void);
void test_obj(void);
void test_render(void);
void test_osl(void);
void test_graph(void);
void test_scenefile(void);
void test_edit(void);
void test_watch(void);
void test_raster(void);
void test_checkpoint(void);
#endif
