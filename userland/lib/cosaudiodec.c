/* cosaudiodec.c - see cosaudiodec.h. MP3 through minimp3, Ogg Vorbis through stb_vorbis, WAV (PCM 8/16/24/32-bit and
 * 32-bit float, any channel count mixed down to two) parsed here. */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "cosaudiodec.h"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "../third_party/minimp3.h"
#define STB_VORBIS_HEADER_ONLY
#include "../third_party/stb_vorbis.c"

enum { K_MP3 = 1, K_OGG, K_WAV };
struct adec {
    int kind;
    const uint8_t *data; size_t len;
    uint32_t rate; double duration;
    /* mp3 */
    mp3dec_t mp3; size_t pos; int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME]; int pcm_frames, pcm_off, pcm_ch;
    /* ogg */
    stb_vorbis *vorb;
    /* wav */
    int channels, bits; bool is_float; size_t data_start, data_end;
};

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static bool open_wav(adec *d) {
    const uint8_t *p = d->data; size_t len = d->len, off = 12;
    uint16_t tag = 0; bool have_fmt = false;
    while (off + 8 <= len) {
        uint32_t cs = rd32(p + off + 4);
        if (!memcmp(p + off, "fmt ", 4) && off + 8 + 16 <= len) {
            tag = (uint16_t)(p[off + 8] | p[off + 9] << 8);
            d->channels = p[off + 10] | p[off + 11] << 8;
            d->rate = rd32(p + off + 12);
            d->bits = p[off + 22] | p[off + 23] << 8;
            if (tag == 0xFFFE && cs >= 26 && off + 8 + 26 <= len) tag = (uint16_t)(p[off + 32] | p[off + 33] << 8);
            have_fmt = true;
        } else if (!memcmp(p + off, "data", 4)) {
            if (!have_fmt) return false;
            d->data_start = off + 8; d->data_end = d->data_start + cs;
            if (d->data_end > len) d->data_end = len;
            break;
        }
        off += 8 + (size_t)cs + (cs & 1);
    }
    if (!have_fmt || !d->data_end) return false;
    d->is_float = tag == 3;
    if (!(tag == 1 || tag == 3) || d->channels < 1 || d->channels > 8) return false;
    if (d->is_float ? d->bits != 32 : !(d->bits == 8 || d->bits == 16 || d->bits == 24 || d->bits == 32)) return false;
    size_t frames = (d->data_end - d->data_start) / (size_t)(d->channels * d->bits / 8);
    d->duration = d->rate ? (double)frames / d->rate : 0;
    d->pos = d->data_start;
    return true;
}

static bool open_mp3(adec *d) {
    mp3dec_init(&d->mp3);
    size_t pos = 0;
    if (d->len > 10 && !memcmp(d->data, "ID3", 3))                     /* skip an ID3v2 tag */
        pos = 10 + (((size_t)d->data[6] & 0x7F) << 21 | ((size_t)d->data[7] & 0x7F) << 14 | ((size_t)d->data[8] & 0x7F) << 7 | ((size_t)d->data[9] & 0x7F));
    d->pos = pos;
    /* walk the frame headers once (no decoding) to get the exact duration and the sample rate */
    mp3dec_t scan; mp3dec_init(&scan);
    size_t p = pos; uint64_t samples = 0; int hz = 0, guard = 0;
    while (p < d->len && guard++ < 2000000) {
        mp3dec_frame_info_t info;
        int n = mp3dec_decode_frame(&scan, d->data + p, (int)(d->len - p > 0x7FFFFFFF ? 0x7FFFFFFF : d->len - p), NULL, &info);
        if (info.frame_bytes <= 0) break;
        if (n > 0) { samples += (uint64_t)n; if (!hz) hz = info.hz; }
        p += (size_t)info.frame_bytes;
    }
    if (!hz) return false;
    d->rate = (uint32_t)hz; d->duration = (double)samples / hz;
    return true;
}

adec *adec_open(const uint8_t *data, size_t len) {
    if (!data || len < 16) return NULL;
    adec *d = (adec *)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->data = data; d->len = len;
    bool ok = false;
    if (!memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WAVE", 4)) { d->kind = K_WAV; ok = open_wav(d); }
    else if (!memcmp(data, "OggS", 4)) {
        d->kind = K_OGG; int err = 0;
        d->vorb = stb_vorbis_open_memory(data, (int)len, &err, NULL);
        if (d->vorb) { stb_vorbis_info vi = stb_vorbis_get_info(d->vorb); d->rate = vi.sample_rate; d->duration = stb_vorbis_stream_length_in_seconds(d->vorb); ok = true; }
    } else { d->kind = K_MP3; ok = open_mp3(d); }
    if (!ok || d->rate < 8000 || d->rate > 48000) { adec_close(d); return NULL; }
    return d;
}

uint32_t adec_rate(const adec *d) { return d->rate; }
double adec_duration(const adec *d) { return d->duration; }
void adec_close(adec *d) { if (!d) return; if (d->vorb) stb_vorbis_close(d->vorb); free(d); }

static int16_t clamp16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

int adec_read(adec *d, int16_t *out, int max_frames) {
    if (d->kind == K_OGG) return stb_vorbis_get_samples_short_interleaved(d->vorb, 2, out, max_frames * 2);
    if (d->kind == K_WAV) {
        int bytes = d->bits / 8, ch = d->channels, n = 0;
        while (n < max_frames && d->pos + (size_t)(ch * bytes) <= d->data_end) {
            int s[8];
            for (int c = 0; c < ch; c++) {
                const uint8_t *q = d->data + d->pos + (size_t)(c * bytes);
                if (d->is_float) { float f; memcpy(&f, q, 4); s[c] = (int)(f * 32767.0f); }
                else if (bytes == 1) s[c] = ((int)q[0] - 128) << 8;
                else if (bytes == 2) s[c] = (int16_t)(q[0] | q[1] << 8);
                else if (bytes == 3) s[c] = (int16_t)(q[1] | q[2] << 8);
                else s[c] = (int16_t)(q[2] | q[3] << 8);
            }
            int l = s[0], r = ch > 1 ? s[1] : s[0];
            if (ch > 2) { l = l * 6 / 10 + s[2] * 4 / 10; r = r * 6 / 10 + s[2] * 4 / 10; }
            out[n * 2] = clamp16(l); out[n * 2 + 1] = clamp16(r);
            d->pos += (size_t)(ch * bytes); n++;
        }
        return n;
    }
    int n = 0;                                                      /* mp3 */
    while (n < max_frames) {
        if (d->pcm_off >= d->pcm_frames) {
            if (d->pos >= d->len) break;
            mp3dec_frame_info_t info;
            int16_t tmp[MINIMP3_MAX_SAMPLES_PER_FRAME];
            int got = mp3dec_decode_frame(&d->mp3, d->data + d->pos, (int)(d->len - d->pos > 0x7FFFFFFF ? 0x7FFFFFFF : d->len - d->pos), tmp, &info);
            if (info.frame_bytes <= 0) { d->pos = d->len; break; }
            d->pos += (size_t)info.frame_bytes;
            if (got <= 0) continue;
            for (int i = 0; i < got; i++) {
                d->pcm[i * 2] = tmp[i * info.channels];
                d->pcm[i * 2 + 1] = tmp[i * info.channels + (info.channels > 1 ? 1 : 0)];
            }
            d->pcm_frames = got; d->pcm_off = 0;
        }
        int take = d->pcm_frames - d->pcm_off; if (take > max_frames - n) take = max_frames - n;
        memcpy(out + n * 2, d->pcm + d->pcm_off * 2, (size_t)take * 4);
        d->pcm_off += take; n += take;
    }
    return n;
}
