#include "string.h"

char* cos_itoa(int value, char* buf, int base) {
    static const char digits[] = "0123456789ABCDEF";
    char tmp[33];
    int i = 0;
    unsigned int v;

    if (!buf) return NULL;
    if (base < 2 || base > 16) {
        buf[0] = '\0';
        return buf;
    }

    if (value < 0 && base == 10) {
        buf[0] = '-';
        v = (unsigned int)(-(long long)value);
        buf++;
    } else {
        v = (unsigned int)value;
    }

    if (v == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return buf;
    }

    while (v && i < (int)sizeof(tmp)) {
        tmp[i++] = digits[v % (unsigned int)base];
        v /= (unsigned int)base;
    }

    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}
