#include "gui/python_code_to_c_calc.h"

void p2c_entry_clear(P2C_Entry *e) {
    if (!e) return;
    e->text[0] = '\0';
    e->len = 0u;
}

int p2c_entry_insert(P2C_Entry *e, char ch) {
    if (!e || e->len >= P2C_ENTRY_CAPACITY) return 0;
    e->text[e->len++] = ch;
    e->text[e->len] = '\0';
    return 1;
}

void p2c_entry_backspace(P2C_Entry *e) {
    if (!e || e->len == 0u) return;
    e->len--;
    e->text[e->len] = '\0';
}

void p2c_entry_set(P2C_Entry *e, const char *s) {
    if (!e) return;
    p2c_entry_clear(e);
    if (!s) return;
    for (size_t i = 0; s[i] && e->len < P2C_ENTRY_CAPACITY; i++) p2c_entry_insert(e, s[i]);
}

void p2c_entry_draw(P2C_Canvas *c, int32_t x, int32_t y, int32_t w, int32_t h,
                    const P2C_Entry *e, uint32_t fg, uint32_t bg) {
    if (!c || !e || w <= 0 || h <= 0) return;
    p2c_fb_fill_rect(c, x, y, w, h, bg);
    p2c_fb_fill_rect(c, x, y, w, 1, fg);
    p2c_fb_fill_rect(c, x, y + h - 1, w, 1, fg);
    p2c_fb_fill_rect(c, x, y, 1, h, fg);
    p2c_fb_fill_rect(c, x + w - 1, y, 1, h, fg);
    int32_t tx = x + 4, ty = y + (h - P2C_GUI_CHAR_HEIGHT) / 2;
    for (uint32_t i = 0; i < e->len; i++) {
        p2c_fb_draw_char(c, tx, ty, (uint8_t)e->text[i], fg);
        tx += P2C_GUI_CHAR_WIDTH;
    }
}

/* ── 式の評価（再帰下降。文法: expr = term {(+|-) term}, term = unary {(*|/) unary},
 *    unary = -unary | primary, primary = number | ( expr )） ── */

typedef struct {
    const char *s;
    size_t pos;
    int error;
} P2C_CalcParser;

static void calc_skip_space(P2C_CalcParser *p) {
    while (p->s[p->pos] == ' ' || p->s[p->pos] == '\t') p->pos++;
}

static double calc_parse_expr(P2C_CalcParser *p);

static double calc_parse_number(P2C_CalcParser *p) {
    double v = 0.0;
    int digits = 0;
    while (p->s[p->pos] >= '0' && p->s[p->pos] <= '9') {
        v = v * 10.0 + (double)(p->s[p->pos] - '0');
        p->pos++;
        digits++;
    }
    if (p->s[p->pos] == '.') {
        p->pos++;
        double scale = 0.1;
        while (p->s[p->pos] >= '0' && p->s[p->pos] <= '9') {
            v += scale * (double)(p->s[p->pos] - '0');
            scale *= 0.1;
            p->pos++;
            digits++;
        }
    }
    if (digits == 0) p->error = 1;
    return v;
}

static double calc_parse_primary(P2C_CalcParser *p) {
    calc_skip_space(p);
    if (p->s[p->pos] == '(') {
        p->pos++;
        double v = calc_parse_expr(p);
        calc_skip_space(p);
        if (p->s[p->pos] != ')') { p->error = 1; return 0.0; }
        p->pos++;
        return v;
    }
    return calc_parse_number(p);
}

static double calc_parse_unary(P2C_CalcParser *p) {
    calc_skip_space(p);
    if (p->s[p->pos] == '-') {
        p->pos++;
        return -calc_parse_unary(p);
    }
    return calc_parse_primary(p);
}

static double calc_parse_term(P2C_CalcParser *p) {
    double v = calc_parse_unary(p);
    for (;;) {
        calc_skip_space(p);
        char op = p->s[p->pos];
        if (op != '*' && op != '/') break;
        p->pos++;
        double r = calc_parse_unary(p);
        if (op == '*') v = v * r;
        else {
            if (!(r < 0.0 || r > 0.0)) { p->error = 1; return 0.0; }   /* 0 除算（NaN も含めて誤り扱い） */
            v = v / r;
        }
        if (p->error) return 0.0;
    }
    return v;
}

static double calc_parse_expr(P2C_CalcParser *p) {
    double v = calc_parse_term(p);
    for (;;) {
        calc_skip_space(p);
        char op = p->s[p->pos];
        if (op != '+' && op != '-') break;
        p->pos++;
        double r = calc_parse_term(p);
        v = (op == '+') ? v + r : v - r;
        if (p->error) return 0.0;
    }
    return v;
}

int p2c_calc_eval(const char *expr, double *out) {
    if (!expr || !out) return -1;
    P2C_CalcParser p = { expr, 0u, 0 };
    double v = calc_parse_expr(&p);
    calc_skip_space(&p);
    if (p.error || p.s[p.pos] != '\0') return -1;
    *out = v;
    return 0;
}

int p2c_calc_format(double value, char *out, size_t cap) {
    if (!out || cap < 2u) return -1;
    if (!(value > -1e15 && value < 1e15)) return -1;   /* 範囲外と NaN */
    char tmp[40];
    size_t n = 0;
    int neg = value < 0.0;
    double a = neg ? -value : value;
    int64_t ip = (int64_t)a;
    double frac = a - (double)ip;
    int64_t fd = (int64_t)(frac * 1000000000.0 + 0.5);
    if (fd >= 1000000000) { ip++; fd -= 1000000000; }
    /* 整数部 */
    char digits[24];
    size_t dn = 0;
    do { digits[dn++] = (char)('0' + (ip % 10)); ip /= 10; } while (ip > 0);
    if (neg && (fd != 0 || dn > 0)) tmp[n++] = '-';
    for (size_t i = dn; i > 0; i--) tmp[n++] = digits[i - 1];
    /* 小数部（末尾の 0 を除く） */
    if (fd > 0) {
        char fr[9];
        for (int i = 8; i >= 0; i--) { fr[i] = (char)('0' + (fd % 10)); fd /= 10; }
        size_t last = 9;
        while (last > 0 && fr[last - 1] == '0') last--;
        tmp[n++] = '.';
        for (size_t i = 0; i < last; i++) tmp[n++] = fr[i];
    }
    if (n + 1u > cap) return -1;
    for (size_t i = 0; i < n; i++) out[i] = tmp[i];   /* string.h に依存しない */
    out[n] = '\0';
    return 0;
}

void p2c_calc_init(P2C_Calc *calc) {
    if (!calc) return;
    p2c_entry_clear(&calc->entry);
    calc->error = 0;
}

void p2c_calc_press(P2C_Calc *calc, const char *key) {
    if (!calc || !key || !key[0]) return;
    char k = key[0];
    if (k == 'C') { p2c_calc_init(calc); return; }
    if (k == 'B') { if (calc->error) p2c_entry_clear(&calc->entry); else p2c_entry_backspace(&calc->entry); calc->error = 0; return; }
    if (k == '=') {
        double v;
        char buf[40];
        if (p2c_calc_eval(calc->entry.text, &v) == 0 && p2c_calc_format(v, buf, sizeof(buf)) == 0) {
            p2c_entry_set(&calc->entry, buf);
            calc->error = 0;
        } else {
            p2c_entry_set(&calc->entry, "Error");
            calc->error = 1;
        }
        return;
    }
    /* 数字・演算子・括弧・小数点: 誤りの後は入力を消してから入れる */
    if (calc->error) { p2c_entry_clear(&calc->entry); calc->error = 0; }
    p2c_entry_insert(&calc->entry, k);
}
