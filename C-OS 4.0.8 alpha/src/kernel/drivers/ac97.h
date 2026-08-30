/**
 * ac97.h - Intel AC'97 (ICH) audio codec driver
 *
 * This is the hardware output path the MP3 player was missing: the
 * mp3 backend decodes real PCM from the MP3 file, but until this driver
 * existed nothing ever sent that PCM to a sound device - it was just
 * buffered in a ring and silently dropped as "played" time elapsed.
 *
 * Targets the AC97 codec QEMU emulates with `-device AC97` (also the
 * real Intel 82801AA/ICH controller), PCI vendor 0x8086 device 0x2415.
 */
#ifndef AC97_H
#define AC97_H

#include "types.h"

/* Probe the PCI bus for an AC97 controller, reset the codec, unmute
 * output, and set up the PCM-out buffer descriptor list. Safe to call
 * even if no AC97 device is present (returns -1, every other ac97_*
 * call then becomes a harmless no-op). */
int ac97_init(void);

/* True once ac97_init() found and set up a working codec. */
bool ac97_is_available(void);

/* Configure the output rate for subsequent ac97_write_samples() calls.
 * Uses Variable Rate Audio (VRA) if the codec supports it so MP3s at
 * 44.1kHz/32kHz/etc. play back at the correct pitch instead of being
 * force-played at a fixed 48kHz. channels must be 1 or 2. */
void ac97_configure(uint32_t sample_rate_hz, uint32_t channels);

/* How many interleaved 16-bit samples can currently be queued without
 * overwriting a buffer the hardware hasn't finished playing yet. */
uint64_t ac97_free_space(void);

/* Queue interleaved 16-bit PCM samples for playback. Returns how many
 * of `count` samples were actually accepted (may be less than count if
 * the ring is momentarily full - caller should retry the remainder). */
uint64_t ac97_write_samples(const int16_t* samples, uint64_t count);

/* Master + PCM-out volume, 0 (silent) - 100 (full volume). */
void ac97_set_volume(uint32_t volume_0_100);

/* Stop DMA and reset the ring (e.g. on stop/track change). */
void ac97_stop(void);

/* Plays a generated square-wave tone - no decoder, no file, just a
 * synthesized waveform straight into ac97_write_samples(). Meant as
 * the simplest possible end-to-end test of the output path above,
 * before trusting it with real decoded audio (WAV, then MP3).
 * duration_ms is capped at 1000 (the ring holds ~1.36s at 48kHz, and
 * this generates and queues the whole tone up front rather than
 * streaming it - fine for a short beep, not meant for long playback).
 * Blocks until fully queued if the ring is briefly full. */
void ac97_beep(uint32_t freq_hz, uint32_t duration_ms);

#endif /* AC97_H */
