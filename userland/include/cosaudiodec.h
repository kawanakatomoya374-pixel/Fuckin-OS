/* cosaudiodec.h - decode an MP3 / Ogg Vorbis / WAV file held in memory into 16-bit stereo PCM, a few frames at a time. */
#ifndef COSAUDIODEC_H
#define COSAUDIODEC_H
#include <stdint.h>
#include <stddef.h>
typedef struct adec adec;
/* `data` must stay valid until adec_close(). The format is detected from the content. Returns NULL for anything else,
 * and for sample rates the audio device cannot take (outside 8000..48000 Hz). */
adec *adec_open(const uint8_t *data, size_t len);
uint32_t adec_rate(const adec *d);
double adec_duration(const adec *d);                   /* seconds (0 if unknown) */
int adec_read(adec *d, int16_t *out, int max_frames);  /* stereo interleaved; returns frames, 0 at the end */
void adec_close(adec *d);
#endif
