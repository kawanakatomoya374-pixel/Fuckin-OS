/* c2py（簡易 C -> Python 変換器）の回帰テスト。
 * 関数・for・printf の変換、未対応の struct/typedef/enum の扱い（TODO と末尾の notes 節）、
 * typedef の宣言子名が式文として漏れないこと（以前の不具合）を検査する。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tools/c2py.h"

static int failures = 0;

static void expect_has(const char *text, const char *needle, const char *label) {
    if (!strstr(text, needle)) {
        fprintf(stderr, "FAIL %s: expected to find \"%s\"\n", label, needle);
        failures++;
    }
}

static void expect_absent(const char *text, const char *needle, const char *label) {
    if (strstr(text, needle)) {
        fprintf(stderr, "FAIL %s: did not expect \"%s\"\n", label, needle);
        failures++;
    }
}

int main(void) {
    const char *src =
        "struct Point { int x; int y; };\n"
        "typedef enum { A, B } Mode;\n"
        "int add(int a, int b) { return a + b; }\n"
        "int main(void) { int s = 0; for (int i = 0; i < 3; i++) { s = s + i; }"
        " printf(\"%d\\n\", s); return 0; }\n";
    char *out = NULL;
    if (c_to_python(src, &out) != 0 || !out) {
        fprintf(stderr, "FAIL c_to_python returned an error\n");
        return 1;
    }
    expect_has(out, "def add(a, b):", "function definition");
    expect_has(out, "return a+b", "return statement");
    expect_has(out, "def main():", "main becomes def main()");
    expect_has(out, "for i in range(0, 3, 1):", "counting for loop");
    expect_has(out, "print(f\"{s}\")", "printf to print");
    expect_has(out, "if __name__ == \"__main__\":", "entry point guard");
    expect_has(out, "TODO(c2py): struct/typedef/enum/union", "unsupported construct marked");
    expect_has(out, "# --- c2py notes", "notes section emitted");
    expect_has(out, "# - struct Point", "note names the struct");
    expect_has(out, "# - typedef enum", "note names the typedef");
    expect_absent(out, "\nMode\n", "typedef declarator must not leak as a statement");
    if (failures) { free(out); return 1; }
    printf("c2py_ok: 10 structural assertions passed\n");
    free(out);
    return 0;
}
