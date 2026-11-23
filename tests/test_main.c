/* Nora - unit test runner: nora_tests [suite ...]. MIT License. */
#include "test.h"
#include <string.h>

int g_test_failures = 0;
int g_test_checks = 0;

typedef struct { const char *name; void (*fn)(void); } suite;
static const suite suites[] = {
    { "spectrum", test_spectrum }, { "bsdf", test_bsdf }, { "bvh", test_bvh }, { "obj", test_obj },
    { "osl", test_osl }, { "graph", test_graph }, { "render", test_render },
    { "scenefile", test_scenefile }, { "edit", test_edit }, { "watch", test_watch }, { "raster", test_raster },
    { "checkpoint", test_checkpoint },
};

int main(int argc, char **argv) {
    int i, j, ran = 0;
    for (i = 0; i < (int)(sizeof suites / sizeof suites[0]); i++) {
        int want = argc < 2;
        for (j = 1; j < argc; j++) if (!strcmp(argv[j], suites[i].name)) want = 1;
        if (!want) continue;
        printf("[%s]\n", suites[i].name);
        suites[i].fn();
        ran++;
    }
    if (!ran) { printf("no such suite\n"); return 2; }
    printf("%d checks, %d failures\n", g_test_checks, g_test_failures);
    return g_test_failures ? 1 : 0;
}
