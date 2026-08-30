/**
 * cos_http_date.c - native implementations of utils/time.h's
 * nsc_sntimet/nsc_snptimet/nsc_strntimet/rfc1123_date.
 *
 * Not a port of utils/time.c (excluded - see PORTING_NOTES.md, it
 * needs curl for HTTP-date parsing): nsc_sntimet/nsc_snptimet turned
 * out to already have a curl-free fallback path upstream (plain
 * snprintf/strtoll, guarded by #ifndef HAVE_STRFTIME/HAVE_STRPTIME,
 * which is exactly the path this kernel wants since it has neither)
 * - reimplemented here rather than fighting the #ifdef machinery to
 * force that path in the original file. nsc_strntimet (parses an
 * RFC 1123 HTTP-date string, e.g. a Last-Modified header) and
 * rfc1123_date (the inverse: formats one) both go through
 * curl_getdate/strftime upstream; RFC 1123's date format is simple
 * and fully specified, so both are hand-written here against it
 * directly instead, using the same civil-calendar algorithm as
 * lib/time_impl.c.
 */
#include "time.h"
#include "string.h"
#include "stdlib.h"
#include "errno.h"
#include "utils/errors.h"

/* Inverse of lib/time_impl.c's cos_civil_from_days: proleptic
 * Gregorian year/month/day -> days since 1970-01-01. Same source
 * (Howard Hinnant's public "chrono-Compatible Low-Level Date
 * Algorithms") as the other direction. */
static long cos_days_from_civil(int y, unsigned m, unsigned d)
{
    long yy = y - (m <= 2 ? 1 : 0);
    long era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);              /* [0, 399] */
    unsigned mp = (m + 9) % 12;                              /* [0, 11] */
    unsigned doy = (153 * mp + 2) / 5 + d - 1;               /* [0, 365] */
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;    /* [0, 146096] */
    return era * 146097 + (long)doe - 719468;
}

static const char *const cos_wdays[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static const char *const cos_months[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

int nsc_sntimet(char *str, size_t size, time_t *timep)
{
    return snprintf(str, size, "%lld", (long long)*timep);
}

nserror nsc_snptimet(const char *str, size_t size, time_t *timep)
{
    if (size < 1) {
        return NSERROR_BAD_PARAMETER;
    }
    char *endp;
    errno = 0;
    long long v = strtoll(str, &endp, 10);
    if (errno != 0 || endp == str) {
        return NSERROR_BAD_PARAMETER;
    }
    *timep = (time_t)v;
    return NSERROR_OK;
}

/* Parses "Wdy, DD Mon YYYY HH:MM:SS GMT" (RFC 1123 / HTTP-date, the
 * format almost every real server sends and the only one this
 * kernel's fetchers need to understand). Deliberately does not also
 * accept RFC 850 or asctime() format (obsolete alternates the HTTP
 * spec allows for compatibility) - nothing in this tree has needed
 * one yet; extend it here if that changes rather than assuming this
 * covers every HTTP date a server could ever send. */
nserror nsc_strntimet(const char *str, size_t size, time_t *timep)
{
    if (str == NULL || timep == NULL) {
        return NSERROR_BAD_PARAMETER;
    }

    /* Skip "Wdy, " (don't validate which weekday - trust the
     * numeric date instead, same as most real-world parsers do). */
    const char *p = str;
    const char *end = str + size;
    while (p < end && *p != ',') p++;
    if (p >= end) return NSERROR_BAD_PARAMETER;
    p++;
    if (p < end && *p == ' ') p++;

    char *endp;
    long day = strtol(p, &endp, 10);
    if (endp == p) return NSERROR_BAD_PARAMETER;
    p = endp;
    if (p < end && *p == ' ') p++;

    int mon = -1;
    for (int i = 0; i < 12; i++) {
        if ((end - p) >= 3 && strncmp(p, cos_months[i], 3) == 0) {
            mon = i;
            break;
        }
    }
    if (mon < 0) return NSERROR_BAD_PARAMETER;
    p += 3;
    if (p < end && *p == ' ') p++;

    long year = strtol(p, &endp, 10);
    if (endp == p) return NSERROR_BAD_PARAMETER;
    p = endp;
    if (p < end && *p == ' ') p++;

    long hour = strtol(p, &endp, 10);
    if (endp == p || (endp < end && *endp != ':')) return NSERROR_BAD_PARAMETER;
    p = endp + 1;
    long minute = strtol(p, &endp, 10);
    if (endp == p || (endp < end && *endp != ':')) return NSERROR_BAD_PARAMETER;
    p = endp + 1;
    long second = strtol(p, &endp, 10);
    if (endp == p) return NSERROR_BAD_PARAMETER;

    long days = cos_days_from_civil((int)year, (unsigned)(mon + 1), (unsigned)day);
    *timep = (time_t)(days * 86400L + hour * 3600L + minute * 60L + second);
    return NSERROR_OK;
}

const char *rfc1123_date(time_t t)
{
    static char buf[32]; /* not reentrant - matches every real-world
                           * rfc1123_date()/asctime()-family function,
                           * none of which are reentrant either
                           * without an _r suffix and an
                           * explicit buffer, and nothing in this
                           * single-threaded kernel overlaps two
                           * calls anyway. */
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT",
             cos_wdays[tm.tm_wday % 7], tm.tm_mday,
             cos_months[tm.tm_mon % 12], tm.tm_year + 1900,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}
