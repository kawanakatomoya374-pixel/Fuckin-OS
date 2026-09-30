/* C-OS Music - ring-3 audio player (MP3 / OGG Vorbis / WAV).
 *
 * Public C-OS interfaces only: window API v2 (cos_win2_*), cos_ui,
 * audio syscalls (cos_audio_*), file descriptors. Decoding is done here:
 * minimp3 (CC0) for MP3, stb_vorbis (public domain) for Ogg Vorbis, a small
 * RIFF parser for WAV. Files are streamed, never loaded whole.
 *
 * Launched by the OS for every "open audio file" (file manager, desktop,
 * Start menu) - see src/kernel/cos_music.c. A running instance receives
 * further files as COS_EV_OPEN events, and dropped files as COS_EV_DROP.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include "cos.h"
#include "cos_ui.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "../../third_party/minimp3.h"
#define STB_VORBIS_HEADER_ONLY
#include "../../third_party/stb_vorbis.c"   /* implementation: vorbis.c */

/* ---- limits ---------------------------------------------------------- */
#define MAX_AUDIO_BYTES (15ull * 1024 * 1024)
#define MAX_WAV_BYTES   (30ull * 1024 * 1024)

/* ---- theme ------------------------------------------------------------ */
#define WIN_W 680
#define WIN_H 460
#define SIDE_W 232
#define C_SIDE     0x151923u
#define C_SIDE_HI  0x1F2533u
#define C_MAIN_BOT 0x0E1117u
#define C_TEXT     0xF2F4F8u
#define C_MUTED    0x8B93A7u
#define C_FAINT    0x2A3142u
#define C_ACCENT   0x5B8CFFu

/* ---- library ------------------------------------------------------------ */
enum { K_MP3, K_OGG, K_WAV };
#define MAX_TRACKS 160
typedef struct { char path[288]; char name[128]; uint64_t size; int kind; } track_t;
static track_t g_tracks[MAX_TRACKS];
static int g_ntracks = 0;
#define MAX_DIRS 12
static char g_dirs[MAX_DIRS][256];
static int g_ndirs = 0;

static bool has_ext(const char *n, const char *e) {
    size_t ln = strlen(n), le = strlen(e);
    return ln > le && strcasecmp(n + ln - le, e) == 0;
}
static int kind_of(const char *n) {
    if (has_ext(n, ".mp3")) return K_MP3;
    if (has_ext(n, ".ogg") || has_ext(n, ".oga")) return K_OGG;
    if (has_ext(n, ".wav")) return K_WAV;
    return -1;
}
static int find_track(const char *path) {
    for (int i = 0; i < g_ntracks; ++i) if (!strcmp(g_tracks[i].path, path)) return i;
    return -1;
}
static int add_track(const char *path, const char *name, uint64_t size) {
    int k = kind_of(name);
    if (k < 0) return -1;
    int i = find_track(path);
    if (i >= 0) return i;
    if (g_ntracks >= MAX_TRACKS) return -1;
    track_t *t = &g_tracks[g_ntracks];
    snprintf(t->path, sizeof t->path, "%s", path);
    snprintf(t->name, sizeof t->name, "%s", name);
    t->size = size;
    t->kind = k;
    return g_ntracks++;
}
static void scan_dir(const char *dir) {
    for (int i = 0; i < g_ndirs; ++i) if (!strcmp(g_dirs[i], dir)) return;
    if (g_ndirs < MAX_DIRS) snprintf(g_dirs[g_ndirs++], 256, "%s", dir);
    int fd = cos_opendir(dir);
    if (fd < 0) return;
    cos_dirent_t de;
    while (cos_readdir(fd, &de) == 1) {           /* names and sizes only - no file is opened */
        if (de.is_dir || kind_of(de.name) < 0) continue;
        char p[288];
        snprintf(p, sizeof p, "%s/%s", strcmp(dir, "/") ? dir : "", de.name);
        add_track(p, de.name, de.size);
    }
    cos_close(fd);
}

/* ---- toast ---------------------------------------------------------------- */
static char g_toast[160];
static uint64_t g_toast_until = 0;
static void toast(const char *msg) {
    snprintf(g_toast, sizeof g_toast, "%s", msg);
    g_toast_until = cos_time_ms() + 3500;
}

/* ---- decoder state ------------------------------------------------------- */
static int g_cur = -1;
static int g_fd = -1;
static bool g_playing = false, g_paused = false, g_eof = false;
static uint64_t g_file_pos = 0, g_audio_start = 0, g_audio_end = 0;
static mp3dec_t g_mp3;
static uint8_t g_in[16384];
static int g_in_fill = 0;
static stb_vorbis *g_vorb = NULL;
static uint32_t g_rate = 44100, g_opened_rate = 0;
static int g_src_channels = 2, g_bitrate_kbps = 0;
static double g_duration = 0.0;
static uint64_t g_frames_written = 0;
static double g_seek_base = 0.0;
static uint16_t g_wav_bits = 16, g_wav_align = 4;
/* WAV: every common encoding. g_wav_tag is the resolved format
 * (EXTENSIBLE is unwrapped to its subformat). */
enum { WF_PCM = 1, WF_MSADPCM = 2, WF_FLOAT = 3, WF_ALAW = 6, WF_MULAW = 7, WF_IMAADPCM = 0x11 };
static uint16_t g_wav_tag = WF_PCM;
static uint16_t g_wav_spb = 0;                 /* ADPCM: samples per block */
static int16_t g_ms_coef1[32], g_ms_coef2[32];
static int g_ms_ncoef = 0;
/* Resampling for rates the AC97 DAC cannot take (> 48 kHz, < 8 kHz). */
static uint32_t g_in_rate = 44100;             /* source rate; g_rate = output rate */
static double g_rs_pos = 0.0;                  /* fractional read position, in input frames */
static int16_t g_rs_prev[2];                   /* last input frame of the previous chunk */
static bool g_rs_have_prev = false;
static int16_t g_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
static int g_pcm_len = 0, g_pcm_off = 0;
static char g_title[160], g_artist[160];

#define VIS_N 1024
#define BANDS 28
static float g_vis_ring[VIS_N];
static int g_vis_pos = 0;
static float g_bands[BANDS];

static int g_volume = 85;
static bool g_shuffle = false;
static int g_repeat = 0;              /* 0 off, 1 all, 2 one */
static int g_list_scroll = 0;
static bool g_drag_seek = false, g_drag_vol = false;
static double g_drag_frac = 0.0;
static int g_hover = -1;

static int64_t g_win;
static cos_win_info_t g_wi;
static cui_canvas g_c;

/* ---- file / tag helpers ------------------------------------------------ */
static int read_at(uint64_t off, void *buf, size_t n) {
    if (cos_lseek(g_fd, (int64_t)off) < 0) return -1;
    size_t got = 0;
    while (got < n) {
        ssize_t r = cos_fd_read(g_fd, (char *)buf + got, n - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    return (int)got;
}

static void set_str(char *dst, size_t cap, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && i + 1 < cap && src[i]; ++i) dst[i] = src[i];
    dst[i] = '\0';
    while (i > 0 && (dst[i - 1] == ' ' || dst[i - 1] == '\0')) dst[--i] = '\0';
}

/* ID3v2 text frame -> UTF-8 (encodings 0 Latin-1, 1/2 UTF-16, 3 UTF-8). */
static void id3_text(char *dst, size_t cap, const uint8_t *p, size_t n) {
    if (n < 1) return;
    uint8_t enc = p[0];
    ++p; --n;
    size_t o = 0;
    if (enc == 3) { set_str(dst, cap, (const char *)p, n); return; }
    if (enc == 0) {
        for (size_t i = 0; i < n && p[i] && o + 3 < cap; ++i) {
            uint8_t ch = p[i];
            if (ch < 0x80) dst[o++] = (char)ch;
            else { dst[o++] = (char)(0xC0 | (ch >> 6)); dst[o++] = (char)(0x80 | (ch & 63)); }
        }
        dst[o] = '\0';
        return;
    }
    bool le = true;
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { le = false; p += 2; n -= 2; }
    else if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { p += 2; n -= 2; }
    else if (enc == 2) le = false;
    for (size_t i = 0; i + 1 < n && o + 4 < cap; i += 2) {
        uint32_t u = le ? (uint32_t)(p[i] | (p[i + 1] << 8)) : (uint32_t)((p[i] << 8) | p[i + 1]);
        if (!u) break;
        if (u < 0x80) dst[o++] = (char)u;
        else if (u < 0x800) { dst[o++] = (char)(0xC0 | (u >> 6)); dst[o++] = (char)(0x80 | (u & 63)); }
        else { dst[o++] = (char)(0xE0 | (u >> 12)); dst[o++] = (char)(0x80 | ((u >> 6) & 63)); dst[o++] = (char)(0x80 | (u & 63)); }
    }
    dst[o] = '\0';
}

static uint64_t parse_id3v2(void) {
    uint8_t h[10];
    if (read_at(0, h, 10) != 10 || memcmp(h, "ID3", 3) != 0) return 0;
    uint32_t size = ((h[6] & 127u) << 21) | ((h[7] & 127u) << 14) | ((h[8] & 127u) << 7) | (h[9] & 127u);
    uint8_t ver = h[3];
    uint32_t cap = size < 65536 ? size : 65536;
    uint8_t *tag = (uint8_t *)malloc(cap);
    if (tag && read_at(10, tag, cap) == (int)cap) {
        uint32_t o = 0;
        while (o + 10 <= cap) {
            const uint8_t *f = tag + o;
            if (!f[0]) break;
            uint32_t fs = ver >= 4 ? (((f[4] & 127u) << 21) | ((f[5] & 127u) << 14) | ((f[6] & 127u) << 7) | (f[7] & 127u))
                                   : ((uint32_t)f[4] << 24 | (uint32_t)f[5] << 16 | (uint32_t)f[6] << 8 | f[7]);
            if (fs == 0 || o + 10 + fs > cap) break;
            if (!memcmp(f, "TIT2", 4)) id3_text(g_title, sizeof g_title, f + 10, fs);
            else if (!memcmp(f, "TPE1", 4)) id3_text(g_artist, sizeof g_artist, f + 10, fs);
            o += 10 + fs;
        }
    }
    free(tag);
    return 10u + size + ((h[5] & 0x10) ? 10u : 0u);
}

static void parse_id3v1(uint64_t file_size) {
    if (file_size < 128) return;
    char t[128];
    if (read_at(file_size - 128, t, 128) != 128 || memcmp(t, "TAG", 3) != 0) return;
    if (!g_title[0]) set_str(g_title, sizeof g_title, t + 3, 30);
    if (!g_artist[0]) set_str(g_artist, sizeof g_artist, t + 33, 30);
    g_audio_end = file_size - 128;
}

static bool parse_wav(void) {
    uint8_t h[12];
    if (read_at(0, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return false;
    uint64_t off = 12;
    bool have_fmt = false;
    uint16_t tag = 0;
    for (int guard = 0; guard < 64; ++guard) {
        uint8_t c[8];
        if (read_at(off, c, 8) != 8) return false;
        uint32_t cs = (uint32_t)c[4] | (uint32_t)c[5] << 8 | (uint32_t)c[6] << 16 | (uint32_t)c[7] << 24;
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t f[128];
            uint32_t n = cs < sizeof f ? cs : (uint32_t)sizeof f;
            if (n < 16 || read_at(off + 8, f, n) != (int)n) return false;
            tag = (uint16_t)(f[0] | f[1] << 8);
            g_src_channels = f[2] | f[3] << 8;
            g_in_rate = (uint32_t)f[4] | (uint32_t)f[5] << 8 | (uint32_t)f[6] << 16 | (uint32_t)f[7] << 24;
            g_wav_align = (uint16_t)(f[12] | f[13] << 8);
            g_wav_bits = (uint16_t)(f[14] | f[15] << 8);
            uint16_t cb = n >= 18 ? (uint16_t)(f[16] | f[17] << 8) : 0;
            if (tag == 0xFFFE && n >= 26) tag = (uint16_t)(f[24] | f[25] << 8);   /* EXTENSIBLE: subformat */
            if ((tag == WF_MSADPCM || tag == WF_IMAADPCM) && cb >= 2 && n >= 20)
                g_wav_spb = (uint16_t)(f[18] | f[19] << 8);
            if (tag == WF_MSADPCM && n >= 22) {
                g_ms_ncoef = f[20] | f[21] << 8;
                if (g_ms_ncoef > 32) g_ms_ncoef = 32;
                for (int i = 0; i < g_ms_ncoef && 22 + i * 4 + 3 < (int)n; ++i) {
                    g_ms_coef1[i] = (int16_t)(f[22 + i * 4] | f[23 + i * 4] << 8);
                    g_ms_coef2[i] = (int16_t)(f[24 + i * 4] | f[25 + i * 4] << 8);
                }
            }
            have_fmt = true;
        } else if (!memcmp(c, "data", 4)) {
            g_audio_start = off + 8;
            g_audio_end = g_audio_start + cs;
            if (g_audio_end > g_tracks[g_cur].size || cs == 0 || cs == 0xFFFFFFFFu)   /* streamed/truncated files */
                g_audio_end = g_tracks[g_cur].size;
            break;
        }
        off += 8 + (uint64_t)cs + (cs & 1u);
    }
    if (!have_fmt || !g_audio_start || !g_src_channels || g_src_channels > 8 || !g_in_rate) return false;
    if (g_in_rate < 4000 || g_in_rate > 384000) return false;
    g_wav_tag = tag;
    switch (tag) {
    case WF_PCM:
        if (g_wav_bits != 8 && g_wav_bits != 16 && g_wav_bits != 24 && g_wav_bits != 32) return false;
        break;
    case WF_FLOAT:
        if (g_wav_bits != 32 && g_wav_bits != 64) return false;
        break;
    case WF_ALAW: case WF_MULAW:
        g_wav_bits = 8;
        break;
    case WF_IMAADPCM:
        if (!g_wav_align) return false;
        if (!g_wav_spb) g_wav_spb = (uint16_t)((g_wav_align - 4 * g_src_channels) * 8 / (4 * g_src_channels) + 1);
        break;
    case WF_MSADPCM:
        if (!g_wav_align) return false;
        if (!g_wav_spb) g_wav_spb = (uint16_t)((g_wav_align - 7 * g_src_channels) * 2 / g_src_channels + 2);
        if (g_ms_ncoef < 7) {                         /* the standard table, if the file omits it */
            static const int16_t c1[7] = { 256, 512, 0, 192, 240, 460, 392 };
            static const int16_t c2[7] = { 0, -256, 0, 64, 0, -208, -232 };
            for (int i = 0; i < 7; ++i) { g_ms_coef1[i] = c1[i]; g_ms_coef2[i] = c2[i]; }
            g_ms_ncoef = 7;
        }
        break;
    default:
        return false;
    }
    if (!g_wav_align) g_wav_align = (uint16_t)(g_src_channels * g_wav_bits / 8);
    double frames;
    if (tag == WF_IMAADPCM || tag == WF_MSADPCM)
        frames = (double)((g_audio_end - g_audio_start) / g_wav_align) * g_wav_spb;
    else
        frames = (double)(g_audio_end - g_audio_start) / g_wav_align;
    g_duration = frames / g_in_rate;
    g_bitrate_kbps = (int)((double)(g_audio_end - g_audio_start) * 8.0 / (g_duration > 0 ? g_duration : 1) / 1000.0);
    /* what the DAC gets: sources above 48 kHz or below 8 kHz are resampled */
    g_rate = g_in_rate > 48000 ? 48000 : (g_in_rate < 8000 ? 8000 : g_in_rate);
    g_rs_pos = 0; g_rs_have_prev = false;
    return true;
}


/* ---- playback -------------------------------------------------------------- */
static void audio_ensure_open(uint32_t rate) {
    if (g_opened_rate == rate) return;
    if (g_opened_rate) cos_audio_close();
    int r = cos_audio_open(rate, 2);
    if (r == 0) { g_opened_rate = rate; cos_audio_set_volume((uint32_t)g_volume); }
    else { g_opened_rate = 0; toast(r == -16 ? "Audio is in use by another program" : "No audio device"); }
}

static void close_decoder(void) {
    if (g_vorb) { stb_vorbis_close(g_vorb); g_vorb = NULL; }
    if (g_fd >= 0) cos_close(g_fd);
    g_fd = -1;
}

static void stop_track(void) {
    close_decoder();
    g_playing = g_paused = false;
    g_pcm_len = g_pcm_off = 0;
    if (g_opened_rate) cos_audio_stop();
    memset(g_bands, 0, sizeof g_bands);
}

static bool start_track(int idx) {
    stop_track();
    if (idx < 0 || idx >= g_ntracks) return false;
    track_t *t = &g_tracks[idx];
    uint64_t limit = t->kind == K_WAV ? MAX_WAV_BYTES : MAX_AUDIO_BYTES;
    if (t->size > limit) {
        char m[160];
        snprintf(m, sizeof m, "%s is %.1f MB - the limit is %d MB", t->name, t->size / 1048576.0, (int)(limit >> 20));
        toast(m);
        g_cur = idx;
        return false;
    }
    g_cur = idx;
    g_title[0] = g_artist[0] = '\0';
    g_duration = 0; g_bitrate_kbps = 0; g_src_channels = 2;
    g_in_fill = 0; g_eof = false;
    g_frames_written = 0; g_seek_base = 0;
    if (t->kind == K_OGG) {
        int err = 0;
        FILE *f = fopen(t->path, "rb");
        if (f) g_vorb = stb_vorbis_open_file(f, 1, &err, NULL);
        if (!g_vorb) { if (f) fclose(f); toast("Could not read this Ogg Vorbis file"); return false; }
        stb_vorbis_info vi = stb_vorbis_get_info(g_vorb);
        g_rate = vi.sample_rate;
        g_src_channels = vi.channels;
        unsigned int len = stb_vorbis_stream_length_in_samples(g_vorb);
        if (len) g_duration = (double)len / g_rate;
        if (!g_bitrate_kbps && g_duration > 0) g_bitrate_kbps = (int)(t->size * 8 / g_duration / 1000);
        stb_vorbis_comment cm = stb_vorbis_get_comment(g_vorb);
        for (int i = 0; i < cm.comment_list_length; ++i) {
            if (!strncasecmp(cm.comment_list[i], "TITLE=", 6)) snprintf(g_title, sizeof g_title, "%s", cm.comment_list[i] + 6);
            else if (!strncasecmp(cm.comment_list[i], "ARTIST=", 7)) snprintf(g_artist, sizeof g_artist, "%s", cm.comment_list[i] + 7);
        }
        audio_ensure_open(g_rate);
    } else {
        g_fd = cos_open(t->path, COS_O_RDONLY);
        if (g_fd < 0) { toast("Could not open the file"); return false; }
        g_audio_start = 0; g_audio_end = t->size;
        if (t->kind == K_WAV) {
            if (!parse_wav()) { close_decoder(); toast("Unsupported WAV encoding"); return false; }
            audio_ensure_open(g_rate);
        } else {
            g_audio_start = parse_id3v2();
            parse_id3v1(t->size);
            mp3dec_init(&g_mp3);
        }
    }
    if (!g_title[0]) {
        snprintf(g_title, sizeof g_title, "%s", t->name);
        char *dot = strrchr(g_title, '.');
        if (dot) *dot = '\0';
    }
    g_file_pos = g_audio_start;
    g_playing = true;
    return true;
}

/* Xing/Info header in the first frame: exact frame count for VBR files. */
static void try_xing(const uint8_t *frame, int frame_bytes, const mp3dec_frame_info_t *info) {
    for (int i = 4; i + 12 < frame_bytes && i < 64; ++i) {
        if (!memcmp(frame + i, "Xing", 4) || !memcmp(frame + i, "Info", 4)) {
            uint32_t flags = (uint32_t)frame[i + 4] << 24 | (uint32_t)frame[i + 5] << 16 | (uint32_t)frame[i + 6] << 8 | frame[i + 7];
            if (flags & 1u) {
                uint32_t frames = (uint32_t)frame[i + 8] << 24 | (uint32_t)frame[i + 9] << 16 | (uint32_t)frame[i + 10] << 8 | frame[i + 11];
                int spf = info->layer == 1 ? 384 : (info->hz <= 24000 ? 576 : 1152);
                g_duration = (double)frames * spf / info->hz;
            }
            return;
        }
    }
}


static void vis_push(const int16_t *st, int frames) {
    for (int i = 0; i < frames; ++i) {
        g_vis_ring[g_vis_pos] = (float)(st[i * 2] + st[i * 2 + 1]) * (0.5f / 32768.0f);
        g_vis_pos = (g_vis_pos + 1) % VIS_N;
    }
}

static bool decode_next(void);

/* ---- WAV sample decoders ---- */
static int16_t alaw_to_s16(uint8_t a) {
    a ^= 0x55;
    int t = (a & 0x0F) << 4, seg = (a & 0x70) >> 4;
    if (seg == 0) t += 8; else t += 0x108;
    if (seg > 1) t <<= seg - 1;
    return (int16_t)((a & 0x80) ? t : -t);
}
static int16_t ulaw_to_s16(uint8_t u) {
    u = (uint8_t)~u;
    int t = ((u & 0x0F) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return (int16_t)((u & 0x80) ? (0x84 - t) : (t - 0x84));
}
static const int ima_index_tab[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int ima_step_tab[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767 };
static int16_t clamp16(int v) { return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

/* One ADPCM block -> interleaved int16 frames (all source channels). */
static int decode_adpcm_block(const uint8_t *b, int len, int16_t *out, int max_frames) {
    int ch = g_src_channels, spb = g_wav_spb;
    if (spb > max_frames) spb = max_frames;
    if (g_wav_tag == WF_IMAADPCM) {
        if (len < 4 * ch) return 0;
        int pred[8], idx[8];
        for (int c = 0; c < ch; ++c) {
            pred[c] = (int16_t)(b[c * 4] | b[c * 4 + 1] << 8);
            idx[c] = b[c * 4 + 2] > 88 ? 88 : b[c * 4 + 2];
            out[c] = (int16_t)pred[c];
        }
        int frames = 1, pos = 4 * ch;
        /* data: per channel, 4 bytes (8 nibbles) at a time, interleaved */
        while (frames < spb && pos + 4 * ch <= len) {
            for (int c = 0; c < ch; ++c) {
                for (int k = 0; k < 8; ++k) {
                    int nib = (b[pos + c * 4 + k / 2] >> ((k & 1) * 4)) & 15;
                    int step = ima_step_tab[idx[c]];
                    int diff = step >> 3;
                    if (nib & 1) diff += step >> 2;
                    if (nib & 2) diff += step >> 1;
                    if (nib & 4) diff += step;
                    pred[c] = (nib & 8) ? pred[c] - diff : pred[c] + diff;
                    if (pred[c] > 32767) pred[c] = 32767;
                    if (pred[c] < -32768) pred[c] = -32768;
                    idx[c] += ima_index_tab[nib];
                    if (idx[c] < 0) idx[c] = 0;
                    if (idx[c] > 88) idx[c] = 88;
                    int f = frames + k;
                    if (f < spb) out[f * ch + c] = (int16_t)pred[c];
                }
            }
            pos += 4 * ch;
            frames += 8;
        }
        return frames < spb ? frames : spb;
    }
    /* MS ADPCM */
    static const int adapt[16] = { 230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230 };
    if (len < 7 * ch) return 0;
    int pi[8], delta[8], s1[8], s2[8];
    int p = 0;
    for (int c = 0; c < ch; ++c) { pi[c] = b[p++]; if (pi[c] >= g_ms_ncoef) pi[c] = 0; }
    for (int c = 0; c < ch; ++c) { delta[c] = (int16_t)(b[p] | b[p + 1] << 8); p += 2; }
    for (int c = 0; c < ch; ++c) { s1[c] = (int16_t)(b[p] | b[p + 1] << 8); p += 2; }
    for (int c = 0; c < ch; ++c) { s2[c] = (int16_t)(b[p] | b[p + 1] << 8); p += 2; }
    for (int c = 0; c < ch; ++c) { out[c] = (int16_t)s2[c]; out[ch + c] = (int16_t)s1[c]; }
    int frames = 2, c = 0;
    for (; p < len && frames < spb; ++p) {
        for (int half = 0; half < 2 && frames < spb; ++half) {
            int nib = half == 0 ? (b[p] >> 4) : (b[p] & 15);
            int snib = nib & 8 ? nib - 16 : nib;
            int predv = (s1[c] * g_ms_coef1[pi[c]] + s2[c] * g_ms_coef2[pi[c]]) / 256 + snib * delta[c];
            int16_t v = clamp16(predv);
            s2[c] = s1[c]; s1[c] = v;
            delta[c] = delta[c] * adapt[nib] / 256;
            if (delta[c] < 16) delta[c] = 16;
            out[frames * ch + c] = v;
            if (++c == ch) { c = 0; ++frames; }
        }
    }
    return frames;
}

static bool decode_next(void) {
    g_pcm_len = g_pcm_off = 0;
    if (g_cur < 0) return false;
    track_t *t = &g_tracks[g_cur];
    if (t->kind == K_OGG) {
        if (!g_vorb) return false;
        int n = stb_vorbis_get_samples_short_interleaved(g_vorb, 2, g_pcm, 1152 * 2);
        if (n <= 0) return false;
        g_pcm_len = n * 2;
        return true;
    }
    if (g_fd < 0) return false;
    if (t->kind == K_WAV) {
        if (g_file_pos >= g_audio_end) return false;
        /* 1) decode one chunk into interleaved int16 per source channel */
        static int16_t src[4096 * 8];
        static uint8_t raw[65536];
        int ch = g_src_channels;
        int frames = 0;
        if (g_wav_tag == WF_IMAADPCM || g_wav_tag == WF_MSADPCM) {
            uint64_t blk = g_wav_align;
            if (blk > sizeof raw) return false;
            if (blk > g_audio_end - g_file_pos) blk = g_audio_end - g_file_pos;
            int got = read_at(g_file_pos, raw, (size_t)blk);
            if (got <= 0) return false;
            g_file_pos += (uint64_t)got;
            frames = decode_adpcm_block(raw, got, src, 4096);
        } else {
            uint64_t want = 1152;
            if (want * g_wav_align > sizeof raw) want = sizeof raw / g_wav_align;
            if (want * g_wav_align > g_audio_end - g_file_pos) want = (g_audio_end - g_file_pos) / g_wav_align;
            if (!want) return false;
            int got = read_at(g_file_pos, raw, (size_t)(want * g_wav_align));
            if (got <= 0) return false;
            frames = got / g_wav_align;
            g_file_pos += (uint64_t)frames * g_wav_align;
            int bps = g_wav_bits / 8;
            for (int f = 0; f < frames; ++f)
                for (int c = 0; c < ch; ++c) {
                    const uint8_t *sp = raw + (size_t)f * g_wav_align + (size_t)c * bps;
                    int32_t v;
                    switch (g_wav_tag) {
                    case WF_ALAW:  v = alaw_to_s16(sp[0]); break;
                    case WF_MULAW: v = ulaw_to_s16(sp[0]); break;
                    case WF_FLOAT: {
                        double d;
                        if (g_wav_bits == 32) { float fl; memcpy(&fl, sp, 4); d = fl; } else memcpy(&d, sp, 8);
                        if (d > 1.0) d = 1.0;
                        if (d < -1.0) d = -1.0;
                        v = (int32_t)(d * 32767.0);
                        break;
                    }
                    default:
                        if (g_wav_bits == 8) v = ((int32_t)sp[0] - 128) << 8;
                        else if (g_wav_bits == 16) v = (int16_t)(sp[0] | sp[1] << 8);
                        else if (g_wav_bits == 24) v = (int32_t)((uint32_t)sp[0] << 8 | (uint32_t)sp[1] << 16 | (uint32_t)sp[2] << 24) >> 16;
                        else v = (int32_t)((uint32_t)sp[0] | (uint32_t)sp[1] << 8 | (uint32_t)sp[2] << 16 | (uint32_t)sp[3] << 24) >> 16;
                    }
                    src[f * ch + c] = (int16_t)v;
                }
        }
        if (frames <= 0) return false;
        /* 2) down-mix to stereo (surround: centre/rears folded in at -3 dB) */
        static int16_t st[4096 * 2];
        for (int f = 0; f < frames; ++f) {
            const int16_t *s0 = &src[f * ch];
            int32_t l, r;
            if (ch == 1) l = r = s0[0];
            else if (ch == 2) { l = s0[0]; r = s0[1]; }
            else {
                l = s0[0]; r = s0[1];
                if (ch >= 3) { l += s0[2] * 7 / 10; r += s0[2] * 7 / 10; }       /* centre */
                if (ch >= 5) { l += s0[4] * 7 / 10; }                          /* rear left (5.1: index 4) */
                if (ch >= 6) { r += s0[5] * 7 / 10; }
                l = l * 6 / 10; r = r * 6 / 10;                                /* headroom */
            }
            st[f * 2] = (int16_t)(l > 32767 ? 32767 : (l < -32768 ? -32768 : l));
            st[f * 2 + 1] = (int16_t)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
        }
        /* 3) resample if the DAC can't take the source rate */
        if (g_in_rate == g_rate) {
            int n = frames > MINIMP3_MAX_SAMPLES_PER_FRAME ? MINIMP3_MAX_SAMPLES_PER_FRAME : frames;
            memcpy(g_pcm, st, (size_t)n * 4);
            g_pcm_len = n * 2;
            return true;
        }
        double step = (double)g_in_rate / g_rate;
        int out = 0;
        while (out < MINIMP3_MAX_SAMPLES_PER_FRAME) {
            int i0 = (int)floor(g_rs_pos);              /* -1 = previous chunk's last frame */
            if (i0 + 1 >= frames) break;
            double fr = g_rs_pos - i0;
            for (int c = 0; c < 2; ++c) {
                int16_t a = (i0 < 0 && g_rs_have_prev) ? g_rs_prev[c] : st[(i0 < 0 ? 0 : i0) * 2 + c];
                int16_t b = st[(i0 + 1) * 2 + c];
                g_pcm[out * 2 + c] = (int16_t)(a + (b - a) * fr);
            }
            ++out;
            g_rs_pos += step;
        }
        g_rs_pos -= frames;                          /* next chunk's frame 0 = this chunk's index `frames` */
        g_rs_prev[0] = st[(frames - 1) * 2];
        g_rs_prev[1] = st[(frames - 1) * 2 + 1];
        g_rs_have_prev = true;
        g_pcm_len = out * 2;
        return out > 0 || decode_next();
    }
    for (int attempt = 0; attempt < 64; ++attempt) {
        if (!g_eof && g_in_fill < (int)sizeof g_in) {   /* keep minimp3's window full */
            uint64_t want = sizeof g_in - (size_t)g_in_fill;
            if (want > g_audio_end - g_file_pos) want = g_audio_end - g_file_pos;
            int got = want ? read_at(g_file_pos, g_in + g_in_fill, (size_t)want) : 0;
            if (got <= 0) g_eof = true;
            else { g_in_fill += got; g_file_pos += (uint64_t)got; }
        }
        if (g_in_fill == 0) return false;
        mp3dec_frame_info_t info;
        mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        int samples = mp3dec_decode_frame(&g_mp3, g_in, g_in_fill, pcm, &info);
        int consumed = info.frame_bytes;
        if (consumed == 0) { if (g_eof) return false; continue; }
        if (samples > 0) {
            if (!g_bitrate_kbps) {
                g_rate = (uint32_t)info.hz;
                g_bitrate_kbps = info.bitrate_kbps;
                g_src_channels = info.channels;
                try_xing(g_in, consumed, &info);
                if (g_duration <= 0 && g_bitrate_kbps > 0)
                    g_duration = (double)(g_audio_end - g_audio_start) * 8.0 / (g_bitrate_kbps * 1000.0);
                audio_ensure_open(g_rate);
            }
            for (int i = 0; i < samples; ++i) {
                g_pcm[i * 2] = pcm[i * info.channels];
                g_pcm[i * 2 + 1] = pcm[i * info.channels + (info.channels > 1 ? 1 : 0)];
            }
            g_pcm_len = samples * 2;
        }
        memmove(g_in, g_in + consumed, (size_t)(g_in_fill - consumed));
        g_in_fill -= consumed;
        if (g_pcm_len) return true;
    }
    return false;
}

static double position_seconds(void) {
    if (g_cur < 0 || !g_rate) return 0;
    cos_audio_status_t st;
    uint64_t pending = 0;
    if (g_opened_rate && cos_audio_status(&st) == 0) pending = st.pending_samples / 2;
    uint64_t heard = g_frames_written > pending ? g_frames_written - pending : 0;
    return g_seek_base + (double)heard / g_rate;
}

static void seek_to(double frac) {
    if (g_cur < 0 || g_duration <= 0) return;
    if (frac < 0) frac = 0;
    if (frac > 0.999) frac = 0.999;
    track_t *t = &g_tracks[g_cur];
    if (t->kind == K_OGG) {
        if (!g_vorb) return;
        stb_vorbis_seek(g_vorb, (unsigned int)(frac * g_duration * g_rate));
    } else if (g_fd >= 0) {
        uint64_t span = g_audio_end - g_audio_start;
        if (t->kind == K_WAV) {
            uint64_t off = (uint64_t)(frac * span);
            off -= off % g_wav_align;               /* frame or ADPCM block boundary */
            g_file_pos = g_audio_start + off;
            g_rs_pos = 0; g_rs_have_prev = false;
        } else {
            g_file_pos = g_audio_start + (uint64_t)(frac * span);
            g_in_fill = 0;
            mp3dec_init(&g_mp3);
        }
    }
    g_eof = false;
    g_pcm_len = g_pcm_off = 0;
    if (g_opened_rate) cos_audio_stop();
    g_seek_base = frac * g_duration;
    g_frames_written = 0;
}

static int pick_next(int dir) {
    if (g_ntracks == 0) return -1;
    if (g_shuffle && g_ntracks > 1) {
        int n;
        do { n = rand() % g_ntracks; } while (n == g_cur);
        return n;
    }
    int n = g_cur + dir;
    if (n >= g_ntracks) return g_repeat == 1 ? 0 : -1;
    if (n < 0) return 0;
    return n;
}

static void pump_audio(void) {
    if (!g_playing || g_paused) return;
    for (int loops = 0; loops < 24; ++loops) {
        if (g_pcm_off >= g_pcm_len) {
            if (!decode_next()) {
                cos_audio_status_t st;
                st.pending_samples = 0;
                if (g_opened_rate) { cos_audio_drain(); cos_audio_status(&st); }
                if (st.pending_samples == 0) {
                    if (g_repeat == 2) { start_track(g_cur); return; }
                    int next = pick_next(+1);
                    if (next >= 0) start_track(next); else stop_track();
                }
                return;
            }
        }
        if (!g_opened_rate) return;
        cos_audio_status_t st;
        cos_audio_status(&st);
        if (st.free_samples < (uint64_t)(g_pcm_len - g_pcm_off)) return;
        int64_t acc = cos_audio_write(g_pcm + g_pcm_off, (uint64_t)(g_pcm_len - g_pcm_off));
        if (acc <= 0) return;
        vis_push(g_pcm + g_pcm_off, (int)acc / 2);
        g_pcm_off += (int)acc;
        g_frames_written += (uint64_t)acc / 2;
    }
}

static void update_bands(void) {
    const int N = 512;
    static float win[512];
    static bool init = false;
    if (!init) { for (int i = 0; i < N; ++i) win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (N - 1)); init = true; }
    float x[512];
    for (int i = 0; i < N; ++i) x[i] = g_vis_ring[(g_vis_pos - N + i + VIS_N) % VIS_N] * win[i];
    for (int b = 0; b < BANDS; ++b) {
        float f = 45.0f * powf(15000.0f / 45.0f, (float)b / (BANDS - 1));
        float k = 2.0f * cosf(2.0f * (float)M_PI * f / (float)(g_rate ? g_rate : 44100));
        float s1 = 0, s2 = 0;
        for (int i = 0; i < N; ++i) { float s0 = x[i] + k * s1 - s2; s2 = s1; s1 = s0; }
        float mag = sqrtf(fmaxf(s1 * s1 + s2 * s2 - k * s1 * s2, 0.0f)) / (N * 0.25f);
        float lvl = (20.0f * log10f(mag + 1e-5f) + 60.0f) / 60.0f;
        if (lvl < 0) lvl = 0;
        if (lvl > 1) lvl = 1;
        g_bands[b] = lvl > g_bands[b] ? lvl : g_bands[b] * 0.84f;
    }
}

/* ---- UI ---------------------------------------------------------------------- */
#define MAIN_X   SIDE_W
#define ART_X    (MAIN_X + 28)
#define ART_Y    28
#define ART_S    176
#define INFO_X   (ART_X + ART_S + 24)
#define VIS_X    (MAIN_X + 28)
#define VIS_W    (WIN_W - MAIN_X - 56)
#define VIS_BASE 272
#define BAR_Y    318
#define CTRL_Y   382
#define LIST_Y   72
#define ROW_H    44
#define VOL_X    (WIN_W - 118)
#define VOL_W    90

enum { B_SHUFFLE, B_PREV, B_PLAY, B_NEXT, B_REPEAT, B_COUNT };
static const int b_cx[B_COUNT] = { MAIN_X + 52, MAIN_X + 110, MAIN_X + 170, MAIN_X + 230, MAIN_X + 288 };
static int b_r(int b) { return b == B_PLAY ? 26 : 18; }

static uint32_t track_color(int idx, int which) {
    if (idx < 0) return which ? 0x6A4CE0u : 0x3B6FF0u;
    uint32_t h = 2166136261u;
    for (const char *p = g_tracks[idx].name; *p; ++p) h = (h ^ (uint8_t)*p) * 16777619u;
    static const uint32_t pal[] = { 0x3B6FF0u, 0x8B4CE0u, 0xE0508Au, 0xF08A3Bu, 0x2FB38Fu, 0x28A7D8u, 0xD8B028u, 0x6A4CE0u };
    return pal[(h >> (which ? 3 : 0)) % 8];
}

static const char *kind_name(int k) { return k == K_MP3 ? "MP3" : k == K_OGG ? "OGG" : "WAV"; }

static void fmt_time(char *buf, size_t n, double s) {
    if (s < 0) s = 0;
    int t = (int)s;
    snprintf(buf, n, "%d:%02d", t / 60, t % 60);
}

static int badge(int x, int y, const char *txt) {
    cui_font *f = cui_font_bold(11);
    int w = cui_text_width(f, txt) + 14;
    cui_round_rect_alpha(&g_c, x, y, w, 20, 10, 0xFFFFFF, 34);
    cui_text_center(&g_c, f, x, y, w, 20, txt, C_TEXT);
    return w;
}

static void draw_icon(int b, bool hover) {
    cui_canvas *c = &g_c;
    int cx = b_cx[b], cy = CTRL_Y;
    if (b == B_PLAY) {
        cui_circle(c, (float)cx, (float)cy, 26, hover ? 0xFFFFFFu : 0xF2F4F8u);
        uint32_t ic = C_MAIN_BOT;
        if (g_playing && !g_paused) {
            cui_round_rect(c, cx - 9, cy - 10, 6, 20, 2, ic);
            cui_round_rect(c, cx + 3, cy - 10, 6, 20, 2, ic);
        } else {
            cui_triangle(c, (float)cx - 6, (float)cy - 11, (float)cx - 6, (float)cy + 11, (float)cx + 12, (float)cy, ic);
        }
        return;
    }
    if (hover) cui_circle(c, (float)cx, (float)cy, 18, 0x2A3142u);
    uint32_t ic = C_TEXT;
    if (b == B_PREV) {
        cui_round_rect(c, cx - 9, cy - 7, 3, 14, 1, ic);
        cui_triangle(c, (float)cx + 8, (float)cy - 7, (float)cx + 8, (float)cy + 7, (float)cx - 5, (float)cy, ic);
    } else if (b == B_NEXT) {
        cui_round_rect(c, cx + 6, cy - 7, 3, 14, 1, ic);
        cui_triangle(c, (float)cx - 8, (float)cy - 7, (float)cx - 8, (float)cy + 7, (float)cx + 5, (float)cy, ic);
    } else if (b == B_SHUFFLE) {
        uint32_t col = g_shuffle ? C_ACCENT : C_MUTED;
        cui_line(c, cx - 9, cy - 5, cx + 7, cy + 5, 1.8f, col);
        cui_line(c, cx - 9, cy + 5, cx + 7, cy - 5, 1.8f, col);
        cui_triangle(c, cx + 5, cy - 9, cx + 10, cy - 4, cx + 4, cy - 2, col);
        cui_triangle(c, cx + 5, cy + 9, cx + 10, cy + 4, cx + 4, cy + 2, col);
        if (g_shuffle) cui_circle(c, (float)cx, (float)cy + 15, 2, C_ACCENT);
    } else if (b == B_REPEAT) {
        uint32_t col = g_repeat ? C_ACCENT : C_MUTED;
        cui_round_rect_outline(c, cx - 10, cy - 6, 20, 12, 5, 1.8f, col);
        cui_triangle(c, cx + 2, cy - 10, cx + 7, cy - 6, cx + 2, cy - 2, col);
        if (g_repeat == 2) cui_text_center(c, cui_font_bold(9), cx - 6, cy - 6, 12, 12, "1", col);
        if (g_repeat) cui_circle(c, (float)cx, (float)cy + 15, 2, C_ACCENT);
    }
}

static void draw(void) {
    cui_canvas *c = &g_c;
    cui_unclip(c);
    uint32_t tc1 = track_color(g_cur, 0), tc2 = track_color(g_cur, 1);

    /* ---- sidebar: library ---- */
    cui_fill(c, 0, 0, SIDE_W, WIN_H, C_SIDE);
    cui_text(c, cui_font_bold(17), 20, 16, "C-OS Music", C_TEXT);
    char lib[48];
    snprintf(lib, sizeof lib, "Library  \xE2\x80\xA2  %d track%s", g_ntracks, g_ntracks == 1 ? "" : "s");
    cui_text(c, cui_font_default(12), 20, 42, lib, C_MUTED);
    cui_clip(c, 0, LIST_Y - 4, SIDE_W, WIN_H - LIST_Y - 34);
    if (g_ntracks == 0) {
        cui_text(c, cui_font_default(12), 20, LIST_Y + 6, "No music found.", C_MUTED);
        cui_text(c, cui_font_default(12), 20, LIST_Y + 24, "Open an .mp3 / .ogg / .wav", C_MUTED);
        cui_text(c, cui_font_default(12), 20, LIST_Y + 42, "from the File Manager.", C_MUTED);
    }
    for (int i = 0; i < g_ntracks; ++i) {
        int y = LIST_Y + (i - g_list_scroll) * ROW_H;
        if (y + ROW_H < LIST_Y - 4 || y > WIN_H) continue;
        bool cur = i == g_cur, hov = g_hover == 100 + i;
        if (cur || hov) cui_round_rect(c, 10, y, SIDE_W - 20, ROW_H - 4, 8, cur ? C_SIDE_HI : 0x1B2029u);
        if (cur) cui_round_rect(c, 10, y + 10, 3, ROW_H - 24, 1, C_ACCENT);
        cui_round_rect(c, 20, y + 6, 28, 28, 7, track_color(i, 0));
        cui_text_center(c, cui_font_default(15), 20, y + 6, 28, 28, "\xE2\x99\xAA", 0xFFFFFF);
        char nm[128];
        snprintf(nm, sizeof nm, "%s", g_tracks[i].name);
        char *dot = strrchr(nm, '.');
        if (dot) *dot = '\0';
        cui_text_fit(c, cur ? cui_font_bold(13) : cui_font_default(13), 58, y + 4, SIDE_W - 76, nm, cur ? C_TEXT : 0xD4D8E2u);
        char meta[48];
        bool big = g_tracks[i].size > (g_tracks[i].kind == K_WAV ? MAX_WAV_BYTES : MAX_AUDIO_BYTES);
        snprintf(meta, sizeof meta, "%s  \xE2\x80\xA2  %.1f MB%s", kind_name(g_tracks[i].kind), g_tracks[i].size / 1048576.0, big ? "  (too large)" : "");
        cui_text(c, cui_font_default(11), 58, y + 22, meta, big ? 0xE06A6Au : C_MUTED);
    }
    cui_unclip(c);
    cui_fill(c, 0, WIN_H - 30, SIDE_W, 30, C_SIDE);
    cui_text(c, cui_font_default(11), 20, WIN_H - 22, "Space  \xE2\x80\xA2  \xE2\x86\x90 \xE2\x86\x92  \xE2\x80\xA2  N / P", 0x5C6477u);

    /* ---- main panel ---- */
    int mw = WIN_W - MAIN_X;
    for (int j = 0; j < WIN_H; ++j)
        cui_fill(c, MAIN_X, j, mw, 1, cui_mix(cui_mix(tc1, C_MAIN_BOT, 150), C_MAIN_BOT, j * 255 / WIN_H));

    /* art */
    cui_round_rect(c, ART_X + 4, ART_Y + 8, ART_S, ART_S, 20, cui_mix(C_MAIN_BOT, 0, 80));   /* soft shadow */
    for (int j = 0; j < ART_S; ++j) {
        uint32_t col = cui_mix(tc1, tc2, j * 255 / ART_S);
        cui_round_rect(c, ART_X, ART_Y + j, ART_S, 1, 0, col);
    }
    /* round the art's corners onto the background */
    for (int j = 0; j < 20; ++j) for (int i = 0; i < 20; ++i) {
        float dx = 20 - i - 0.5f, dy = 20 - j - 0.5f, d = sqrtf(dx * dx + dy * dy) - 20;
        int a = d > 0.5f ? 255 : (d < -0.5f ? 0 : (int)((d + 0.5f) * 255));
        if (!a) continue;
        int xs[4] = { ART_X + i, ART_X + ART_S - 1 - i, ART_X + i, ART_X + ART_S - 1 - i };
        int ys[4] = { ART_Y + j, ART_Y + j, ART_Y + ART_S - 1 - j, ART_Y + ART_S - 1 - j };
        for (int k = 0; k < 4; ++k) {
            uint32_t bg = cui_mix(cui_mix(tc1, C_MAIN_BOT, 150), C_MAIN_BOT, ys[k] * 255 / WIN_H);
            uint32_t *p = &c->px[ys[k] * c->stride + xs[k]];
            *p = cui_mix(*p, bg, a);
        }
    }
    cui_text_center(c, cui_font_default(80), ART_X, ART_Y, ART_S, ART_S, "\xE2\x99\xAA", 0xFFFFFF);

    /* info */
    int iw = WIN_W - INFO_X - 20;
    cui_text(c, cui_font_bold(10), INFO_X, ART_Y + 8, g_playing ? (g_paused ? "PAUSED" : "NOW PLAYING") : "READY", C_ACCENT);
    cui_text_fit(c, cui_font_bold(20), INFO_X, ART_Y + 26, iw, g_cur >= 0 ? g_title : "Nothing playing", C_TEXT);
    cui_text_fit(c, cui_font_default(14), INFO_X, ART_Y + 56, iw,
                 g_cur >= 0 ? (g_artist[0] ? g_artist : "Unknown artist") : "Pick a song from the library", C_MUTED);
    if (g_cur >= 0 && g_playing) {
        int bx = INFO_X;
        char b1[24], b2[24];
        bx += badge(bx, ART_Y + 90, kind_name(g_tracks[g_cur].kind)) + 6;
        if (g_bitrate_kbps) { snprintf(b1, sizeof b1, "%d kbps", g_bitrate_kbps); bx += badge(bx, ART_Y + 90, b1) + 6; }
        snprintf(b2, sizeof b2, "%.1f kHz", g_rate / 1000.0);
        badge(bx, ART_Y + 90, b2);
        badge(INFO_X, ART_Y + 118, g_src_channels >= 2 ? "Stereo" : "Mono");
    }

    /* visualizer (bars + faint reflection) */
    int bw = VIS_W / BANDS;
    for (int b = 0; b < BANDS; ++b) {
        int h = (int)(g_bands[b] * 50);
        if (h < 3) h = 3;
        int x = VIS_X + b * bw;
        uint32_t col = cui_mix(tc1, 0xFFFFFFu, 60 + b * 4);
        cui_round_rect(c, x + 2, VIS_BASE - h, bw - 4, h, 3, col);
        int rh = h * 35 / 100;
        cui_round_rect_alpha(c, x + 2, VIS_BASE + 3, bw - 4, rh, 3, col, 55);
    }

    /* progress */
    double pos = g_drag_seek ? g_drag_frac * g_duration : position_seconds();
    double frac = g_duration > 0 ? pos / g_duration : 0;
    if (frac > 1) frac = 1;
    if (frac < 0) frac = 0;
    cui_round_rect_alpha(c, VIS_X, BAR_Y, VIS_W, 5, 2, 0xFFFFFF, 40);
    cui_round_rect(c, VIS_X, BAR_Y, (int)(VIS_W * frac), 5, 2, 0xFFFFFF);
    if (g_cur >= 0) cui_circle(c, VIS_X + (float)(VIS_W * frac), BAR_Y + 2.5f, 7, 0xFFFFFF);
    char t1[16], t2[16];
    fmt_time(t1, sizeof t1, pos);
    fmt_time(t2, sizeof t2, g_duration);
    cui_text(c, cui_font_default(12), VIS_X, BAR_Y + 12, t1, C_MUTED);
    cui_text(c, cui_font_default(12), VIS_X + VIS_W - cui_text_width(cui_font_default(12), t2), BAR_Y + 12, t2, C_MUTED);

    /* controls */
    for (int b = 0; b < B_COUNT; ++b) draw_icon(b, g_hover == b);

    /* volume (this app's own level; the keyboard volume keys set the OS master) */
    int vy = CTRL_Y;
    cui_fill(c, VOL_X - 22, vy - 3, 4, 6, C_MUTED);
    cui_triangle(c, VOL_X - 18, vy - 3, VOL_X - 18, vy + 3, VOL_X - 11, vy, C_MUTED);
    cui_triangle(c, VOL_X - 18, vy - 7, VOL_X - 18, vy + 7, VOL_X - 12, vy, C_MUTED);
    cui_round_rect_alpha(c, VOL_X, vy - 2, VOL_W, 4, 2, 0xFFFFFF, 40);
    cui_round_rect(c, VOL_X, vy - 2, VOL_W * g_volume / 100, 4, 2, 0xFFFFFF);
    cui_circle(c, VOL_X + VOL_W * g_volume / 100.0f, (float)vy, 6, 0xFFFFFF);

    /* toast */
    if (g_toast[0] && cos_time_ms() < g_toast_until) {
        cui_font *f = cui_font_default(12);
        int tw = cui_text_width(f, g_toast) + 28;
        if (tw > mw - 30) tw = mw - 30;
        int tx = MAIN_X + (mw - tw) / 2;
        cui_round_rect_alpha(c, tx, WIN_H - 44, tw, 30, 15, 0x000000, 200);
        cui_text_fit(c, f, tx + 14, WIN_H - 36, tw - 28, g_toast, C_TEXT);
    }
    cos_win2_present(g_win);
}

/* ---- input -------------------------------------------------------------------- */
static int hit(int x, int y) {
    for (int b = 0; b < B_COUNT; ++b) {
        int dx = x - b_cx[b], dy = y - CTRL_Y, r = b_r(b);
        if (dx * dx + dy * dy <= r * r) return b;
    }
    if (x < SIDE_W && y >= LIST_Y && y < WIN_H - 30) {
        int row = (y - LIST_Y) / ROW_H + g_list_scroll;
        if (row >= 0 && row < g_ntracks) return 100 + row;
    }
    return -1;
}

static void set_volume(int v) {
    g_volume = v < 0 ? 0 : (v > 100 ? 100 : v);
    if (g_opened_rate) cos_audio_set_volume((uint32_t)g_volume);
}

static void press(int b) {
    if (b == B_PLAY) {
        if (!g_playing) { if (g_ntracks) start_track(g_cur >= 0 ? g_cur : 0); }
        else if (!g_paused) {
            double pos = position_seconds();
            g_paused = true;
            if (g_opened_rate) cos_audio_stop();
            if (g_duration > 0) seek_to(pos / g_duration);
        } else g_paused = false;
    } else if (b == B_PREV) {
        if (position_seconds() > 3 && g_cur >= 0) start_track(g_cur);
        else { int p = g_cur > 0 ? g_cur - 1 : 0; start_track(p); }
    } else if (b == B_NEXT) {
        int n = pick_next(+1);
        start_track(n >= 0 ? n : 0);
    } else if (b == B_SHUFFLE) {
        g_shuffle = !g_shuffle;
        toast(g_shuffle ? "Shuffle on" : "Shuffle off");
    } else if (b == B_REPEAT) {
        g_repeat = (g_repeat + 1) % 3;
        toast(g_repeat == 0 ? "Repeat off" : g_repeat == 1 ? "Repeat all" : "Repeat one");
    }
}

static void open_path(const char *path) {
    if (!path || !path[0]) return;
    const char *slash = strrchr(path, '/');
    char dir[256];
    if (slash && slash != path) { size_t n = (size_t)(slash - path); if (n >= sizeof dir) n = sizeof dir - 1; memcpy(dir, path, n); dir[n] = '\0'; }
    else snprintf(dir, sizeof dir, "/");
    scan_dir(dir);                           /* its folder joins the library */
    int idx = find_track(path);
    if (idx < 0) {
        const char *name = slash ? slash + 1 : path;
        if (kind_of(name) < 0) { toast("Not an audio file (MP3, OGG and WAV are supported)"); return; }
        cos_stat_t st;
        uint64_t size = cos_stat(path, &st) == 0 ? st.size : 0;
        idx = add_track(path, name, size);
    }
    if (idx >= 0) {
        start_track(idx);
        int vis = (WIN_H - LIST_Y - 34) / ROW_H;
        if (idx < g_list_scroll || idx >= g_list_scroll + vis) g_list_scroll = idx > 1 ? idx - 1 : 0;
    }
}

static bool handle(const cos_win_event_t *e) {
    switch (e->type) {
    case COS_EV_CLOSE: return false;
    case COS_EV_OPEN:
    case COS_EV_DROP: {
        char p[256];
        if (cos_win2_get_path(g_win, p, sizeof p) > 0) open_path(p);
        return true;
    }
    case COS_EV_KEY: {
        char k = e->ascii;
        if (k == ' ') press(B_PLAY);
        else if (k == 'n' || k == 'N') press(B_NEXT);
        else if (k == 'p' || k == 'P') press(B_PREV);
        else if (k == 's' || k == 'S') press(B_SHUFFLE);
        else if (k == 'r' || k == 'R') press(B_REPEAT);
        else if (e->special == COS_KEY_RIGHT && g_duration > 0) seek_to((position_seconds() + 5) / g_duration);
        else if (e->special == COS_KEY_LEFT && g_duration > 0) seek_to((position_seconds() - 5) / g_duration);
        else if (e->special == COS_KEY_UP) set_volume(g_volume + 5);
        else if (e->special == COS_KEY_DOWN) set_volume(g_volume - 5);
        return true;
    }
    case COS_EV_MOUSE_MOVE:
        g_hover = hit(e->x, e->y);
        if (g_drag_seek) { g_drag_frac = (double)(e->x - VIS_X) / VIS_W; if (g_drag_frac < 0) g_drag_frac = 0; if (g_drag_frac > 1) g_drag_frac = 1; }
        if (g_drag_vol) set_volume((e->x - VOL_X) * 100 / VOL_W);
        return true;
    case COS_EV_MOUSE_DOWN: {
        if (e->button != 1) return true;
        int h = hit(e->x, e->y);
        if (h >= 0 && h < B_COUNT) { press(h); return true; }
        if (h >= 100) { start_track(h - 100); return true; }
        if (e->x >= VIS_X - 6 && e->x <= VIS_X + VIS_W + 6 && e->y >= BAR_Y - 8 && e->y <= BAR_Y + 12 && g_cur >= 0) {
            g_drag_seek = true; g_drag_frac = (double)(e->x - VIS_X) / VIS_W; return true;
        }
        if (e->x >= VOL_X - 6 && e->x <= VOL_X + VOL_W + 6 && e->y >= CTRL_Y - 10 && e->y <= CTRL_Y + 10) {
            g_drag_vol = true; set_volume((e->x - VOL_X) * 100 / VOL_W); return true;
        }
        return true;
    }
    case COS_EV_MOUSE_UP:
        if (e->button == 1) {
            if (g_drag_seek) { g_drag_seek = false; seek_to(g_drag_frac); }
            g_drag_vol = false;
        }
        return true;
    case COS_EV_WHEEL:
        if (e->x < SIDE_W) {
            g_list_scroll -= e->wheel;
            int vis = (WIN_H - LIST_Y - 34) / ROW_H;
            int max = g_ntracks - vis;
            if (g_list_scroll > max) g_list_scroll = max;
            if (g_list_scroll < 0) g_list_scroll = 0;
        } else set_volume(g_volume + e->wheel * 5);
        return true;
    }
    return true;
}

int main(int argc, char **argv) {
    srand((unsigned)cos_time_ms());
    scan_dir("/desktop");
    scan_dir("/music");
    g_win = cos_win2_create("Music", WIN_W, WIN_H, &g_wi);
    if (g_win <= 0) { printf("[music] no window\n"); return 1; }
    cui_canvas_init(&g_c, g_wi.pixels, g_wi.width, g_wi.height, g_wi.stride);
    printf("[music] ready: %d track(s)\n", g_ntracks);
    if (argc > 1 && argv[1]) open_path(argv[1]);
#ifdef MUSIC_AUTOPLAY
    for (int i = 0; i < g_ntracks; ++i) if (strstr(g_tracks[i].name, MUSIC_AUTOPLAY)) { start_track(i); break; }
#endif
    draw();
    uint64_t last = cos_time_ms();
    bool toast_shown = false;
    for (;;) {
        cos_win_event_t ev;
        bool dirty = false;
        int got = cos_win2_wait(g_win, &ev, g_playing && !g_paused ? 5 : 40);
        while (got == 1) {
            if (!handle(&ev)) goto quit;
            dirty = true;
            got = cos_win2_poll(g_win, &ev);
        }
        pump_audio();
        uint64_t now = cos_time_ms();
        bool toast_live = g_toast[0] && now < g_toast_until;
        if (toast_live != toast_shown) { dirty = true; toast_shown = toast_live; }
        if (dirty || (g_playing && !g_paused && now - last >= 66)) {
            if (g_playing && !g_paused) update_bands();
            else for (int b = 0; b < BANDS; ++b) g_bands[b] *= 0.8f;
            draw();
            last = now;
        }
    }
quit:
    stop_track();
    if (g_opened_rate) cos_audio_close();
    cos_win2_close(g_win);
    return 0;
}
