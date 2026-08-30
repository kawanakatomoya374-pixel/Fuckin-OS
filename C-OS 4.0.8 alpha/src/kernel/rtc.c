/**
 * rtc.c - Real Time Clock Implementation
 * C-OS 4.0.8 alpha Full CMOS RTC read implementation
 *
 * Reads actual time from CMOS chip via I/O ports 0x70/0x71.
 * Handles BCD-to-binary conversion and 12/24-hour mode.
 */
#include "types.h"
#include "io.h"
#include "serial.h"
#include "irq.h"
#include "rtc.h"

/* ---- I/O ports ---- */
#define RTC_PORT        0x70
#define RTC_DATA        0x71

/* ---- CMOS register indices ---- */
#define CMOS_SECONDS    0x00
#define CMOS_MINUTES    0x02
#define CMOS_HOURS      0x04
#define CMOS_WEEKDAY    0x06
#define CMOS_DAY        0x07
#define CMOS_MONTH      0x08
#define CMOS_YEAR       0x09
#define CMOS_CENTURY    0x32
#define CMOS_STATUS_A   0x0A
#define CMOS_STATUS_B   0x0B
#define CMOS_STATUS_C   0x0C

/* ---- Status register B flags ---- */
#define RTC_SB_24H      0x02
#define RTC_SB_BIN      0x04
#define RTC_SB_PIE      0x40

/* ---- Status register A flags ---- */
#define RTC_SA_UIP      0x80

/* ---- State ---- */
static bool rtc_initialized = false;
static uint64_t rtc_ticks   = 0;

/* ------------------------------------------------------------------ */
static uint8_t cmos_read(uint8_t reg)
{
    outb(RTC_PORT, reg | 0x80);
    return inb(RTC_DATA);
}

static void cmos_write(uint8_t reg, uint8_t val)
{
    outb(RTC_PORT, reg | 0x80);
    outb(RTC_DATA, val);
}

static void rtc_wait_ready(void)
{
    int timeout = 100000;
    while ((cmos_read(CMOS_STATUS_A) & RTC_SA_UIP) && timeout-- > 0)
        ;
}

static uint8_t bcd2bin(uint8_t bcd)
{
    return (bcd & 0x0F) + ((bcd >> 4) * 10);
}

/* ------------------------------------------------------------------ */
rtc_time_t rtc_get_datetime(void)
{
    rtc_time_t t1, t2;
    uint8_t status_b = cmos_read(CMOS_STATUS_B);
    bool is_binary = (status_b & RTC_SB_BIN) != 0;
    bool is_24h    = (status_b & RTC_SB_24H) != 0;

    do {
        rtc_wait_ready();
        t1.second = cmos_read(CMOS_SECONDS);
        t1.minute = cmos_read(CMOS_MINUTES);
        t1.hour   = cmos_read(CMOS_HOURS);
        t1.day    = cmos_read(CMOS_DAY);
        t1.month  = cmos_read(CMOS_MONTH);
        t1.year   = cmos_read(CMOS_YEAR);

        rtc_wait_ready();
        t2.second = cmos_read(CMOS_SECONDS);
        t2.minute = cmos_read(CMOS_MINUTES);
        t2.hour   = cmos_read(CMOS_HOURS);
        t2.day    = cmos_read(CMOS_DAY);
        t2.month  = cmos_read(CMOS_MONTH);
        t2.year   = cmos_read(CMOS_YEAR);
    } while (t1.second != t2.second || t1.minute != t2.minute || t1.hour != t2.hour);

    if (!is_binary) {
        t1.second = bcd2bin(t1.second);
        t1.minute = bcd2bin(t1.minute);
        uint8_t pm = (!is_24h) ? (t1.hour & 0x80) : 0;
        t1.hour   = bcd2bin(t1.hour & 0x7F);
        if (!is_24h) {
            if (pm && t1.hour != 12) t1.hour += 12;
            if (!pm && t1.hour == 12) t1.hour = 0;
        }
        t1.day    = bcd2bin(t1.day);
        t1.month  = bcd2bin(t1.month);
        t1.year   = bcd2bin(t1.year);
    } else if (!is_24h) {
        uint8_t pm = t1.hour & 0x80;
        t1.hour &= 0x7F;
        if (pm && t1.hour != 12) t1.hour += 12;
        if (!pm && t1.hour == 12) t1.hour = 0;
    }

    uint64_t full_year = t1.year;
    if (full_year < 100) {
        uint8_t century_raw = cmos_read(CMOS_CENTURY);
        uint8_t century = is_binary ? century_raw : bcd2bin(century_raw);
        if (century >= 19 && century <= 21)
            full_year += (uint64_t)century * 100;
        else
            full_year += 2000;
    }
    t1.year = full_year;
    return t1;
}

uint64_t rtc_get_time(void)
{
    rtc_time_t t = rtc_get_datetime();
    return (uint64_t)t.hour * 3600 + (uint64_t)t.minute * 60 + t.second;
}

uint64_t rtc_get_seconds_since_epoch(void)
{
    rtc_time_t t = rtc_get_datetime();
    uint64_t years = (t.year > 1970) ? (t.year - 1970) : 0;
    uint64_t days  = years * 365 + years / 4;
    static const uint64_t mdays[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    if (t.month >= 1 && t.month <= 12) days += mdays[t.month - 1];
    days += (t.day > 0) ? (t.day - 1) : 0;
    return days * 86400 + (uint64_t)t.hour * 3600 + (uint64_t)t.minute * 60 + t.second;
}

void rtc_irq_handler(struct regs *r)
{
    (void)r;
    cmos_read(CMOS_STATUS_C);
    rtc_ticks++;
}

void rtc_init(void)
{
    serial_puts("[RTC] Initializing Real Time Clock (CMOS)\n");
    uint8_t sb = cmos_read(CMOS_STATUS_B);
    sb |= RTC_SB_PIE;
    sb |= RTC_SB_24H;
    cmos_write(CMOS_STATUS_B, sb);
    cmos_read(CMOS_STATUS_C);
    outb(RTC_PORT, 0x00);
    rtc_ticks = 0;
    rtc_initialized = true;

    rtc_time_t t = rtc_get_datetime();
    serial_puts("[RTC] Current time: ");
    serial_putdec(t.hour);   serial_puts(":");
    serial_putdec(t.minute); serial_puts(":");
    serial_putdec(t.second); serial_puts("\n");
    serial_puts("[RTC] Current date: ");
    serial_putdec(t.year);  serial_puts("-");
    serial_putdec(t.month); serial_puts("-");
    serial_putdec(t.day);   serial_puts("\n");
    serial_puts("[RTC] RTC initialized\n");
}

bool rtc_is_initialized(void) { return rtc_initialized; }
uint64_t rtc_get_ticks(void)  { return rtc_ticks; }

void rtc_get_current_time(uint8_t* hour, uint8_t* minute, uint8_t* second)
{
    rtc_time_t t = rtc_get_datetime();
    if (hour)   *hour   = t.hour;
    if (minute) *minute = t.minute;
    if (second) *second = t.second;
}

void rtc_get_current_date(uint8_t* day, uint8_t* month, uint8_t* year)
{
    rtc_time_t t = rtc_get_datetime();
    if (day)   *day   = t.day;
    if (month) *month = t.month;
    if (year)  *year  = (uint8_t)(t.year % 100);
}
