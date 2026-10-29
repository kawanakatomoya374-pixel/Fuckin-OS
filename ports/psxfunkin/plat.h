/* The platform layer of the PSXFunkin port: everything the engine backend (backend/*.c) needs from the system below it.
 * cos/plat_cos.c implements it for C-OS (window, keyboard, audio device, microsecond clock); host/plat_host.c is a headless version for
 * testing on a PC (frames are written as PPM files, time is simulated, keys come from a script). */
#ifndef PSXF_PLAT_H
#define PSXF_PLAT_H
#include <stdint.h>

#define PLAT_W 320
#define PLAT_H 240

uint64_t plat_time_us(void);                         /* monotonic microseconds */
void     plat_sleep_until_us(uint64_t t);            /* returns when plat_time_us() >= t (the host version only advances its clock) */
void     plat_present(const uint32_t *argb);         /* PLAT_W x PLAT_H pixels, 0xAARRGGBB */
int      plat_running(void);                         /* 0 once the window is closed */
uint32_t plat_keys(void);                            /* the PSX pad buttons (PAD_* bits of boot/pad.h) that are held right now, including a key pressed and released since the last call */
int      plat_audio_open(int rate);                  /* 0 = ok */
int      plat_audio_write(const int16_t *stereo, int frames);   /* frames accepted (it never blocks) */
int      plat_audio_pending(void);                   /* frames written and not yet heard */
void     plat_audio_stop(void);
const char *plat_data_dir(void);                     /* where the converted game data lives, no trailing slash */
void     plat_log(const char *s);
#endif
