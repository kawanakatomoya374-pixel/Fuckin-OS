/* Ogg Vorbis decoding in a unit of its own (stb_vorbis and minimp3 both define a static get_bits()). */
#include <stdint.h>
#include <stdlib.h>
#define STB_VORBIS_NO_STDIO
#include "stb_vorbis.c"
int16_t *psxf_decode_ogg(const uint8_t *d, size_t n, size_t *frames)
{
	int ch2, rate; short *pcm; int len = stb_vorbis_decode_memory(d, (int)n, &ch2, &rate, &pcm);
	if (len <= 0) { *frames = 0; return NULL; }
	int16_t *out = (int16_t*)malloc((size_t)len * 2 * sizeof(int16_t));
	for (int i = 0; i < len; ++i) { out[i * 2] = pcm[i * ch2]; out[i * 2 + 1] = pcm[i * ch2 + (ch2 > 1 ? 1 : 0)]; }
	free(pcm); *frames = (size_t)len; return out;
}
