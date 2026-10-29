/* Headless platform for testing the port on a PC: simulated 60 Hz clock, frames written as PPM files (PSXF_FRAMES = list of frame numbers, PSXF_OUT = directory),
 * keys from PSXF_KEYS ("frame:mask,frame:mask,...": the pad mask held from that frame on), a fake audio device that plays 735 frames per video frame. */
#include "../plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t now_us; static int frame, running = 1, pending, max_frames = 600;
static uint32_t keys; static int next_key_i; static char keybuf[4096];
void plat_set_data_dir(const char *d) { (void)d; }
void plat_check_data(void) {}
const char *plat_data_dir(void) { const char *d = getenv("PSXF_DATA"); return d ? d : "/home/user/psxf/data"; }
uint64_t plat_time_us(void) { return now_us; }
void plat_sleep_until_us(uint64_t t) { if (t > now_us) now_us = t; }
int plat_running(void) { return running; }
void plat_log(const char *s) { fputs(s, stdout); }
int plat_audio_open(int rate) { (void)rate; return 0; }
int plat_audio_write(const int16_t *p, int n) { (void)p; if (pending + n > 8192) n = 8192 - pending; if (n < 0) n = 0; pending += n; return n; }
int plat_audio_pending(void) { return pending; }
void plat_audio_stop(void) { pending = 0; }
static int wanted(int f) { const char *l = getenv("PSXF_FRAMES"); if (!l) return f == 1 || f % 100 == 0; for (const char *p = l; *p; ) { if (atoi(p) == f) return 1; while (*p && *p != ',') p++; if (*p == ',') p++; } return 0; }
void plat_present(const uint32_t *argb)
{
	frame++; now_us += 16667; pending -= 735; if (pending < 0) pending = 0;
	const char *mf = getenv("PSXF_MAXFRAMES"); if (mf) max_frames = atoi(mf);
	if (frame >= max_frames) running = 0;
	if (wanted(frame)) {
		char path[300]; const char *od = getenv("PSXF_OUT"); snprintf(path, sizeof path, "%s/f%05d.ppm", od ? od : "/tmp/psxf_out", frame);
		FILE *f = fopen(path, "wb"); if (f) { fprintf(f, "P6\n%d %d\n255\n", PLAT_W, PLAT_H); for (int i = 0; i < PLAT_W * PLAT_H; ++i) { fputc((argb[i] >> 16) & 255, f); fputc((argb[i] >> 8) & 255, f); fputc(argb[i] & 255, f); } fclose(f); }
	}
}
uint32_t plat_keys(void)
{
	const char *k = getenv("PSXF_KEYS");
	if (k) { for (const char *p = k; *p; ) { int f = atoi(p); const char *c = strchr(p, ':'); if (!c) break; uint32_t m = (uint32_t)strtoul(c + 1, NULL, 0); if (f <= frame) keys = m; const char *n = strchr(c, ','); if (!n) break; p = n + 1; } }
	return keys;
}
