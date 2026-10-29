#include <time.h>
#ifndef TIME_UTC
#define TIME_UTC 1
#include <stdint.h>
static inline int timespec_get(struct timespec *ts, int base) { (void)base; extern uint64_t cos_time_ms(void); extern int clock_gettime(int, struct timespec *); return clock_gettime(0, ts) == 0 ? base : 0; }
#endif
