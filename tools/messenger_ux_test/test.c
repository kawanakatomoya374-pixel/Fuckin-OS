/* host test for the Message app's pure helpers (copied out of main.c by tools/messenger_ux_test.sh) */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
static uint64_t g_now;
static uint64_t cos_time_unix(void) { return g_now; }
static const char *msg_body(const char *t) {
    if (t[0] == 1 && t[1] == 'Q') { const char *s = strchr(t, 2); if (s) return s + 1; }
    return t;
}
static bool msg_quote(const char *t, char *name, size_t nn, char *snip, size_t ns) {
    if (t[0] != 1 || t[1] != 'Q') return false;
    const char *u = strchr(t + 2, 0x1f), *e = strchr(t, 2);
    if (!u || !e || u > e) return false;
    size_t a = (size_t)(u - (t + 2)), b = (size_t)(e - (u + 1));
    if (a >= nn) a = nn - 1;
    if (b >= ns) b = ns - 1;
    memcpy(name, t + 2, a); name[a] = 0; memcpy(snip, u + 1, b); snip[b] = 0;
    return true;
}
/* copy up to n bytes of one line, dropping control bytes and never cutting a UTF-8 character */
static void clean_snip(char *out, size_t cap, const char *src, int maxb) {
    size_t o = 0; const unsigned char *p = (const unsigned char *)src;
    for (; *p && (int)o < maxb && o + 1 < cap; p++) {
        if (*p < 32) { if ((*p == '\n' || *p == '\t') && o && out[o - 1] != ' ') out[o++] = ' '; continue; }
        out[o++] = (char)*p;
    }
    if (*p && (*p & 0xC0) == 0x80) { while (o > 0 && ((unsigned char)out[o - 1] & 0xC0) == 0x80) o--; if (o > 0) o--; }   /* cut inside a character */
    out[o] = 0;
}

static bool first_url(const char *t, char *out, size_t cap) {
    const char *p = strstr(t, "https://"); if (!p) p = strstr(t, "http://"); if (!p) return false;
    size_t n = 0; while (p[n] && (unsigned char)p[n] > ' ' && n + 1 < cap) n++;
    memcpy(out, p, n); out[n] = 0; return true;
}
/* days since 1970-01-01 -> civil date (Howard Hinnant's algorithm) */
static void civil_from_days(int64_t z, int *y, int *m, int *d) {
    z += 719468; int64_t era = (z >= 0 ? z : z - 146096) / 146097; unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1); *m = (int)(mp < 10 ? mp + 3 : mp - 9); *y = (int)(yy + (*m <= 2));
}
static void fmt_day(uint32_t ts, char *out, size_t n) {
    int64_t day = ts / 86400, today = (int64_t)(cos_time_unix() / 86400);
    if (day == today) { snprintf(out, n, "今日"); return; }
    if (day == today - 1) { snprintf(out, n, "昨日"); return; }
    int y, m, d; civil_from_days(day, &y, &m, &d); int ty, tm, td; civil_from_days(today, &ty, &tm, &td);
    static const char *const wd[7] = { "日", "月", "火", "水", "木", "金", "土" };
    if (y == ty) snprintf(out, n, "%d月%d日(%s)", m, d, wd[(day + 4) % 7]);
    else snprintf(out, n, "%d年%d月%d日(%s)", y, m, d, wd[(day + 4) % 7]);
}

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s\n", #c); fails++; } } while (0)
int main(void) {
    int y, m, d;
    civil_from_days(0, &y, &m, &d); CHECK(y == 1970 && m == 1 && d == 1);
    civil_from_days(20732, &y, &m, &d); CHECK(y == 2026 && m == 10 && d == 6);
    civil_from_days(11016, &y, &m, &d); CHECK(y == 2000 && m == 2 && d == 29);
    civil_from_days(19782, &y, &m, &d); CHECK(y == 2024 && m == 2 && d == 29);
    civil_from_days(19783, &y, &m, &d); CHECK(y == 2024 && m == 3 && d == 1);
    char b[64];
    g_now = 1791314353;                                   /* 2026-10-06 19:19 UTC, a Tuesday */
    fmt_day(1791314353, b, sizeof b); CHECK(!strcmp(b, "今日"));
    fmt_day(1791314353 - 86400, b, sizeof b); CHECK(!strcmp(b, "昨日"));
    fmt_day(1791314353 - 2 * 86400, b, sizeof b); CHECK(!strcmp(b, "10月4日(日)"));
    fmt_day(1791314353 - 400 * 86400, b, sizeof b); CHECK(!strcmp(b, "2025年9月1日(月)"));
    char n[24], s[64];
    CHECK(msg_quote("\x01QBob\x1fhello\x02hi there", n, sizeof n, s, sizeof s) && !strcmp(n, "Bob") && !strcmp(s, "hello"));
    CHECK(!strcmp(msg_body("\x01QBob\x1fhello\x02hi there"), "hi there"));
    CHECK(!strcmp(msg_body("plain"), "plain") && !msg_quote("plain", n, sizeof n, s, sizeof s));
    CHECK(!msg_quote("\x01Qbroken", n, sizeof n, s, sizeof s));
    clean_snip(s, sizeof s, "line one\nline two", 40); CHECK(!strcmp(s, "line one line two"));
    clean_snip(s, sizeof s, "あいうえお", 7); CHECK(!strcmp(s, "あい"));       /* 7 bytes would cut the third character */
    clean_snip(s, sizeof s, "abc\x01\x02" "def", 40); CHECK(!strcmp(s, "abcdef"));
    char u[64]; CHECK(first_url("see https://x.org/a?b=1 now", u, sizeof u) && !strcmp(u, "https://x.org/a?b=1"));
    CHECK(!first_url("no link", u, sizeof u));
    puts(fails ? "FAILED" : "ALL PASS"); return fails != 0;
}
