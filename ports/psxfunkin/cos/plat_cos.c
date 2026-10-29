/* PSXFunkin on C-OS: the platform layer (plat.h). The game is drawn in a 320x240 buffer; the window shows it enlarged 2x (nearest neighbour, the pixel art stays sharp),
 * F11 goes full screen (the compositor then scales it to the screen). The pad is the keyboard (cos_win2_input: the keys down right now, including a tap shorter than a frame):
 *   arrows / W A S D   the d-pad (the four note lanes)       Enter, Esc   START (confirm, pause)
 *   Backspace          CIRCLE (back in the menus)           Tab           SELECT
 * The game's data lives in /apps/psxfunkin/data (or the directory given as the first argument of the program). */
#include "cos.h"
#include "../plat.h"
#include <stdio.h>
#include <string.h>

#define PAD_SELECT 1
#define PAD_START 8
#define PAD_UP 16
#define PAD_RIGHT 32
#define PAD_DOWN 64
#define PAD_LEFT 128
#define PAD_CIRCLE 8192

#define SCALE 2
static int64_t win; static cos_win_info_t info; static int running = 1, have_win;
static uint64_t next_frame_us;
static const char *data_dir = "/apps/psxfunkin/data";

void plat_set_data_dir(const char *d) { if (d && *d) data_dir = d; }
/* no data yet (first start): hand over to the data setup, which downloads and prepares it, and ends this start */
void plat_check_data(void) {
    char p[256]; snprintf(p, sizeof p, "%s/FILES.TXT", data_dir);
    cos_stat_t st; if (cos_stat(p, &st) == 0) return;
    static const char setup[] = "/apps/psxfunkin/setup/psxsetup.c-os";
    const char *av[3] = { setup, "go", NULL };
    if (cos_stat(setup, &st) == 0) cos_spawn_argv(setup, av, NULL);
    exit(0);
}
const char *plat_data_dir(void) { return data_dir; }
uint64_t plat_time_us(void) { return cos_time_us(); }
void plat_log(const char *s) { cos_puts(s); }
int plat_running(void) { return running; }
void plat_sleep_until_us(uint64_t t)
{
	for (;;) {
		uint64_t now = cos_time_us(); if (now >= t) return;
		uint64_t left = t - now;
		if (left > 2000) cos_sleep_ms((left - 1500) / 1000); else { /* the last millisecond or two are spent looking at the clock */ }
	}
}

static void open_window(void)
{
	win = cos_win2_create("PSXFunkin", PLAT_W * SCALE, PLAT_H * SCALE, &info);
	if (win < 0) { cos_puts("[psxf] no window\n"); running = 0; return; }
	have_win = 1; memset(info.pixels, 0, (size_t)info.stride * (size_t)info.height * 4);
}

void plat_present(const uint32_t *argb)
{
	if (!have_win) open_window();
	if (!have_win) return;
	cos_win_event_t e;
	while (cos_win2_poll(win, &e) > 0) {
		if (e.type == COS_EV_CLOSE) running = 0;
		else if (e.type == COS_EV_KEY && e.special == COS_KEY_F1 + 10) cos_win2_fullscreen(win, 2);
	}
	int w = info.width < PLAT_W * SCALE ? info.width : PLAT_W * SCALE, h = info.height < PLAT_H * SCALE ? info.height : PLAT_H * SCALE;
	for (int y = 0; y < h; ++y) {
		const uint32_t *src = argb + (y / SCALE) * PLAT_W; uint32_t *dst = info.pixels + (size_t)y * (size_t)info.stride;
		for (int x = 0; x < w; ++x) dst[x] = src[x / SCALE];
	}
	cos_win2_present(win);
	/* 60 frames a second: wait for the next frame boundary (a late frame does not make the following ones faster) */
	uint64_t now = cos_time_us();
	if (next_frame_us == 0 || now > next_frame_us + 100000) next_frame_us = now;
	next_frame_us += 16667;
	plat_sleep_until_us(next_frame_us);
}

uint32_t plat_keys(void)
{
	if (!have_win) return 0;
	cos_win_input_t in; if (cos_win2_input(win, &in) != 0) return 0;
	uint32_t k = 0;
	if (COS_KEY_DOWN_P(in, COS_SC_UP) || COS_KEY_DOWN_P(in, COS_SC_W)) k |= PAD_UP;
	if (COS_KEY_DOWN_P(in, COS_SC_DOWN) || COS_KEY_DOWN_P(in, COS_SC_S)) k |= PAD_DOWN;
	if (COS_KEY_DOWN_P(in, COS_SC_LEFT) || COS_KEY_DOWN_P(in, COS_SC_A)) k |= PAD_LEFT;
	if (COS_KEY_DOWN_P(in, COS_SC_RIGHT) || COS_KEY_DOWN_P(in, COS_SC_D)) k |= PAD_RIGHT;
	if (COS_KEY_DOWN_P(in, COS_SC_ENTER) || COS_KEY_DOWN_P(in, COS_SC_ESC)) k |= PAD_START;
	if (COS_KEY_DOWN_P(in, COS_SC_BACKSPACE)) k |= PAD_CIRCLE;
	if (COS_KEY_DOWN_P(in, COS_SC_TAB)) k |= PAD_SELECT;
	return k;
}

static int audio_ok;
int plat_audio_open(int rate) { audio_ok = cos_audio_open((uint32_t)rate, 2) == 0; if (!audio_ok) cos_puts("[psxf] no audio device\n"); return audio_ok ? 0 : -1; }
int plat_audio_write(const int16_t *stereo, int frames)
{
	if (!audio_ok) return frames;                                    /* no device: behave as if it played, the game keeps its time */
	int64_t n = cos_audio_write(stereo, (uint64_t)frames * 2); return n < 0 ? 0 : (int)(n / 2);
}
int plat_audio_pending(void)
{
	if (!audio_ok) return 0;
	cos_audio_status_t st; if (cos_audio_status(&st) != 0) return 0;
	return (int)(st.pending_samples / 2);
}
void plat_audio_stop(void) { if (audio_ok) cos_audio_stop(); }
