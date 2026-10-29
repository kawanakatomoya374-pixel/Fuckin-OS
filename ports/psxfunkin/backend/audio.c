/* PSXFunkin on C-OS: music. A song is a ".mus" description (the text file the PlayStation tools turn into a CD-XA stream): one line per channel,
 *   "File.mp3" left_weight right_weight
 * a channel is the mono mix of that file; the game sets the volume of every channel to a speaker (Audio_SetVolume 0x3FFF = full). The files are decoded
 * when the song is loaded. Audio_Pump() runs once per frame and keeps the audio device about 60 ms ahead; the time the game steers by (Audio_GetTime) is the
 * position of what is being heard right now: frames written minus frames still waiting in the device. */
#include "audio.h"
#include "timer.h"
#include "io.h"
#include "main.h"
#include "../plat.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"
int16_t *psxf_decode_ogg(const uint8_t *d, size_t n, size_t *frames);

#define RATE 44100
#define LEAD_FRAMES 2646            /* 60 ms */
#define MAX_CH 4

typedef struct { int16_t *pcm; size_t frames; } Song_File;
static Song_File sfile[MAX_CH]; static int nfiles;
static struct { int file; int wl, wr; int vl, vr; } ch[MAX_CH]; static int nch;
static size_t pos;                 /* next frame to mix */
static uint64_t written;           /* frames given to the device since the music started */
static int state;                  /* 0 stopped, 1 loaded, 2 playing */
static boolean looping;

static int16_t *decode_mp3(const uint8_t *d, size_t n, size_t *frames)
{
	mp3dec_t dec; mp3dec_init(&dec); mp3dec_frame_info_t info; size_t cap = 1 << 20, len = 0; int16_t *out = (int16_t*)malloc(cap * 2 * sizeof(int16_t)); int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
	while (n) {
		int s = mp3dec_decode_frame(&dec, d, (int)n, pcm, &info);
		if (info.frame_bytes <= 0) break;
		d += info.frame_bytes; n -= (size_t)info.frame_bytes;
		for (int i = 0; i < s; ++i) {
			if (len >= cap) { cap *= 2; out = (int16_t*)realloc(out, cap * 2 * sizeof(int16_t)); }
			out[len * 2] = pcm[i * info.channels]; out[len * 2 + 1] = pcm[i * info.channels + (info.channels > 1 ? 1 : 0)]; len++;
		}
	}
	*frames = len; return out;
}
static int load_audio(const char *dir, const char *name)
{
	char full[300]; snprintf(full, sizeof full, "%s/%s/%s", plat_data_dir(), dir, name);
	FILE *f = fopen(full, "rb"); if (!f) { printf("[psxf] music file %s is missing\n", full); return -1; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); uint8_t *b = (uint8_t*)malloc((size_t)n); size_t rd = fread(b, 1, (size_t)n, f); fclose(f);
	size_t fr = 0; int16_t *pcm = NULL; size_t l = strlen(name);
	if (l > 4 && !strcmp(name + l - 4, ".ogg")) pcm = psxf_decode_ogg(b, rd, &fr); else pcm = decode_mp3(b, rd, &fr);
	free(b); if (!pcm) return -1;
	sfile[nfiles].pcm = pcm; sfile[nfiles].frames = fr; return nfiles++;
}

void Audio_Init(void) { plat_audio_open(RATE); state = 0; }
void Audio_Quit(void) { Audio_StopMus(); }

void Audio_StopMus(void)
{
	state = 0; plat_audio_stop();
	for (int i = 0; i < nfiles; ++i) free(sfile[i].pcm);
	nfiles = nch = 0; pos = 0; written = 0;
}

void Audio_LoadMus(const char *path)
{
	extern const char *PSXF_LookupFile(const char *iso);
	Audio_StopMus();
	const char *rel = PSXF_LookupFile(path); if (!rel) { sprintf(error_msg, "[Audio_LoadMus] %s not found", path); ErrorLock(); }
	char full[300]; snprintf(full, sizeof full, "%s/%s", plat_data_dir(), rel);
	FILE *f = fopen(full, "rb"); if (!f) { sprintf(error_msg, "[Audio_LoadMus] cannot open %.200s", full); ErrorLock(); }
	char text[2048]; size_t n = fread(text, 1, sizeof text - 1, f); fclose(f); text[n] = 0;
	char dir[200]; snprintf(dir, sizeof dir, "%s", rel); char *sl = strrchr(dir, '/'); if (sl) *sl = 0; else dir[0] = 0;
	int cnt = -1; char names[MAX_CH][64]; int nn = 0;
	for (char *l = text; l && *l; ) {
		char *nl = strchr(l, '\n'); if (nl) *nl = 0;
		if (cnt < 0) { cnt = atoi(l); }
		else if (*l == '"' && nch < MAX_CH) {
			char *q = strchr(l + 1, '"'); if (q) {
				*q = 0; double wl = 0, wr = 0; sscanf(q + 1, "%lf %lf", &wl, &wr);
				int idx = -1; for (int i = 0; i < nn; ++i) if (!strcmp(names[i], l + 1)) idx = i;
				if (idx < 0) { idx = load_audio(dir, l + 1); if (idx < 0) { sprintf(error_msg, "[Audio_LoadMus] cannot decode %.40s", l + 1); ErrorLock(); } snprintf(names[nn++], 64, "%s", l + 1); }
				ch[nch].file = idx; ch[nch].wl = (int)(wl * 256); ch[nch].wr = (int)(wr * 256); ch[nch].vl = ch[nch].vr = 0; nch++;
			}
		}
		l = nl ? nl + 1 : NULL;
	}
	pos = 0; written = 0; state = 1;
}
void Audio_LoadMusFile(CdlFILE *file) { Audio_LoadMus(file->path); }

void Audio_PlayMus(boolean loops) { looping = loops; pos = 0; written = 0; state = 2; Audio_Pump(); }

void Audio_SetVolume(u8 i, u16 vol_left, u16 vol_right)
{
	if (i < nch) { ch[i].vl = vol_left > 0x3FFF ? 0x3FFF : vol_left; ch[i].vr = vol_right > 0x3FFF ? 0x3FFF : vol_right; }
}

void Audio_Pump(void)
{
	if (state != 2) return;
	int pend = plat_audio_pending();
	while (pend < LEAD_FRAMES) {
		int16_t buf[1024 * 2]; int n = 1024;
		size_t total = 0; for (int c = 0; c < nch; ++c) if (sfile[ch[c].file].frames > total) total = sfile[ch[c].file].frames;
		if (pos >= total) { if (looping && total) pos = 0; else { state = 3; break; } }
		if (pos + (size_t)n > total) n = (int)(total - pos);
		for (int i = 0; i < n; ++i) {
			int l = 0, r = 0;
			for (int c = 0; c < nch; ++c) {
				const Song_File *sf = &sfile[ch[c].file]; if (pos + (size_t)i >= sf->frames) continue;
				int mono = (sf->pcm[(pos + (size_t)i) * 2] * ch[c].wl + sf->pcm[(pos + (size_t)i) * 2 + 1] * ch[c].wr) >> 8;
				l += (mono * ch[c].vl) >> 14; r += (mono * ch[c].vr) >> 14;
			}
			buf[i * 2] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l); buf[i * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
		}
		int acc = plat_audio_write(buf, n);
		if (acc <= 0) break;
		pos += (size_t)acc; written += (uint64_t)acc; pend += acc;
	}
}

fixed_t Audio_GetTime(void)
{
	if (state < 2) return 0;
	int64_t played = (int64_t)written - plat_audio_pending(); if (played < 0) played = 0;
	if (looping) { size_t total = 0; for (int c = 0; c < nch; ++c) if (sfile[ch[c].file].frames > total) total = sfile[ch[c].file].frames; if (total) played %= (int64_t)total; }      /* a looping song starts counting again with every round */
	return (fixed_t)(((uint64_t)played * FIXED_UNIT) / RATE);
}
boolean Audio_IsPlaying(void) { return state == 2 || (state == 3 && plat_audio_pending() > 0); }
