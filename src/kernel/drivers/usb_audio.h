/**
 * usb_audio.h - USB Audio Class 1.0 playback (AudioStreaming isochronous OUT) for xHCI-attached devices.
 *
 * Presents the same kind of sink as ac97.h: signed 16-bit interleaved stereo in, at any rate from 8 to 48 kHz
 * (linearly resampled to the device's own rate). usb_audio_pump() is driven from usb_poll() and keeps a short queue of
 * 1 ms packets on the isochronous endpoint.
 */
#ifndef USB_AUDIO_H
#define USB_AUDIO_H

#include "types.h"
#include <stdbool.h>

bool     usb_audio_ready(void);                         /* a device is configured and its endpoint is streaming-capable */
void     usb_audio_configure(uint32_t rate_hz);         /* rate of the samples that usb_audio_write() receives */
uint64_t usb_audio_write(const int16_t *stereo, uint64_t samples);   /* returns samples (int16 values) accepted */
uint64_t usb_audio_free_space(void);                    /* in int16 values */
uint64_t usb_audio_pending_samples(void);               /* int16 values still to be sent */
uint64_t usb_audio_underruns(void);
void     usb_audio_flush(void);                         /* waits (pumping) until everything queued has been sent */
void     usb_audio_stop(void);                          /* drops queued audio */
void     usb_audio_pump(void);
void     usb_audio_set_volume(uint32_t vol_0_100);
void     usb_audio_get_stats(uint32_t *packets_sent, uint32_t *packets_failed);

#endif
