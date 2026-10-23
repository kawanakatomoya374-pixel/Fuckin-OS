/* 電卓の部品の単体テスト（式評価、表示、キー押下、入力欄）。 */
#include <stdio.h>
#include <string.h>

#include "gui/python_code_to_c_calc.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s\n", msg); failures++; } else { printf("ok   %s\n", msg); } } while (0)

static int eval_is(const char *expr, double expect) {
    double v = 0.0;
    if (p2c_calc_eval(expr, &v) != 0) return 0;
    double d = v - expect;
    return d < 1e-9 && d > -1e-9;
}

static void test_eval(void) {
    CHECK(eval_is("2+3*4", 14.0), "precedence: 2+3*4 = 14");
    CHECK(eval_is("(2+3)*4", 20.0), "parentheses: (2+3)*4 = 20");
    CHECK(eval_is("-3+5", 2.0), "unary minus: -3+5 = 2");
    CHECK(eval_is("2*-3", -6.0), "unary minus after an operator");
    CHECK(eval_is("--2", 2.0), "double unary minus");
    CHECK(eval_is("10/4", 2.5), "division yields a fraction: 10/4 = 2.5");
    CHECK(eval_is("1.5*2", 3.0), "decimal numbers: 1.5*2 = 3");
    CHECK(eval_is(".5+.5", 1.0), "leading decimal point: .5+.5 = 1");
    CHECK(eval_is(" 7 - 2 * ( 3 + 1 ) ", -1.0), "spaces are ignored");
    CHECK(eval_is("100/10/5", 2.0), "division is left-associative");
    CHECK(eval_is("10-4-3", 3.0), "subtraction is left-associative");
    double v;
    CHECK(p2c_calc_eval("1/0", &v) < 0, "division by zero is an error");
    CHECK(p2c_calc_eval("2+", &v) < 0, "dangling operator is an error");
    CHECK(p2c_calc_eval("(1+2", &v) < 0, "unclosed parenthesis is an error");
    CHECK(p2c_calc_eval("", &v) < 0, "empty expression is an error");
    CHECK(p2c_calc_eval("3 4", &v) < 0, "two numbers without an operator is an error");
    CHECK(p2c_calc_eval("2^3", &v) < 0, "unsupported operator is an error");
    CHECK(p2c_calc_eval("1..2", &v) < 0, "two decimal points are an error");
}

static void test_format(void) {
    char b[40];
    CHECK(p2c_calc_format(14.0, b, sizeof b) == 0 && strcmp(b, "14") == 0, "integer 14 prints as 14");
    CHECK(p2c_calc_format(2.5, b, sizeof b) == 0 && strcmp(b, "2.5") == 0, "2.5 prints as 2.5");
    CHECK(p2c_calc_format(-6.0, b, sizeof b) == 0 && strcmp(b, "-6") == 0, "-6 prints as -6");
    CHECK(p2c_calc_format(0.25, b, sizeof b) == 0 && strcmp(b, "0.25") == 0, "0.25 prints as 0.25");
    CHECK(p2c_calc_format(-0.5, b, sizeof b) == 0 && strcmp(b, "-0.5") == 0, "-0.5 prints as -0.5");
    CHECK(p2c_calc_format(0.1 + 0.2, b, sizeof b) == 0 && strcmp(b, "0.3") == 0, "0.1+0.2 prints as 0.3 (9 decimals)");
    CHECK(p2c_calc_format(1.0 / 3.0, b, sizeof b) == 0 && strcmp(b, "0.333333333") == 0, "1/3 prints with 9 decimals");
    CHECK(p2c_calc_format(0.0, b, sizeof b) == 0 && strcmp(b, "0") == 0, "zero prints as 0");
    CHECK(p2c_calc_format(1e15, b, sizeof b) < 0, "values of 1e15 and above are rejected");
    CHECK(p2c_calc_format(14.0, b, 2u) < 0, "output buffer too small is rejected");
}

static void press_all(P2C_Calc *c, const char *keys[], int n) {
    for (int i = 0; i < n; i++) p2c_calc_press(c, keys[i]);
}

static void test_calc_keys(void) {
    P2C_Calc c;
    p2c_calc_init(&c);
    const char *k1[] = { "7", "+", "8", "=" };
    press_all(&c, k1, 4);
    CHECK(strcmp(c.entry.text, "15") == 0, "7 + 8 = shows 15");
    const char *k2[] = { "C", "1", "/", "4", "=" };
    press_all(&c, k2, 5);
    CHECK(strcmp(c.entry.text, "0.25") == 0, "1 / 4 = shows 0.25");
    const char *k3[] = { "C", "1", "/", "0", "=" };
    press_all(&c, k3, 5);
    CHECK(strcmp(c.entry.text, "Error") == 0 && c.error == 1, "1 / 0 = shows Error");
    const char *k4[] = { "5" };
    press_all(&c, k4, 1);
    CHECK(strcmp(c.entry.text, "5") == 0 && c.error == 0, "a key after Error starts a new entry");
    const char *k5[] = { "C", "1", "2", "3", "B" };
    press_all(&c, k5, 5);
    CHECK(strcmp(c.entry.text, "12") == 0, "B removes the last character");
    const char *k6[] = { "C" };
    press_all(&c, k6, 1);
    CHECK(c.entry.len == 0u, "C clears the entry");
    const char *k7[] = { "(", "1", "+", "2", ")", "*", "3", "=" };
    press_all(&c, k7, 8);
    CHECK(strcmp(c.entry.text, "9") == 0, "(1 + 2) * 3 = shows 9");
}

static void test_entry_limits(void) {
    P2C_Entry e;
    p2c_entry_clear(&e);
    int accepted = 0;
    for (int i = 0; i < 70; i++) accepted += p2c_entry_insert(&e, '1');
    CHECK(accepted == P2C_ENTRY_CAPACITY && e.len == P2C_ENTRY_CAPACITY, "entry holds at most 63 characters");
    p2c_entry_backspace(&e);
    CHECK(e.len == P2C_ENTRY_CAPACITY - 1u, "backspace shortens by one");
}

static void test_entry_draw(void) {
    P2C_Canvas cv;
    CHECK(p2c_fb_open_memory(&cv, 96u, 32u, 0u) == 0, "open canvas for the entry");
    P2C_Entry e;
    p2c_entry_set(&e, "12");
    p2c_entry_draw(&cv, 0, 0, 96, 32, &e, 0x00FFFFFFu, 0x00000000u);
    CHECK(p2c_fb_get_pixel(&cv, 0, 0) == 0x00FFFFFFu, "entry border uses the text colour");
    int text = 0;
    for (uint32_t y = 8; y < 24; y++) for (uint32_t x = 4; x < 20; x++)
        if (p2c_fb_get_pixel(&cv, x, y) == 0x00FFFFFFu) text++;
    CHECK(text > 0, "entry text is drawn");
    p2c_fb_close(&cv);
}

int main(void) {
    test_eval();
    test_format();
    test_calc_keys();
    test_entry_limits();
    test_entry_draw();
    if (failures == 0) printf("calc_core_ok\n");
    return failures == 0 ? 0 : 1;
}
