#ifndef COS_AUDIO_H
#define COS_AUDIO_H
#include "types.h"
/* Layout shared with userland (userland/include/cos.h: cos_audio_status_t). */
typedef struct {
    uint32_t available;        /* an audio device exists */
    uint32_t owned_by_caller;  /* the calling process owns it */
    uint32_t busy;             /* another process owns it */
    uint32_t rate;
    uint32_t channels;
    uint32_t reserved;
    uint64_t free_samples;     /* how many samples a write can accept now */
    uint64_t pending_samples;  /* written but not yet heard */
    uint64_t underruns;
} cos_audio_status_t;
uint32_t cos_audio_owner(void);
int64_t cos_audio_open(uint32_t pid, uint32_t rate, uint32_t channels);
int64_t cos_audio_write(uint32_t pid, const int16_t* pcm, uint64_t samples);
int64_t cos_audio_status(uint32_t pid, cos_audio_status_t* out);
int64_t cos_audio_drain(uint32_t pid);
int64_t cos_audio_stop(uint32_t pid);
int64_t cos_audio_set_volume(uint32_t pid, uint32_t vol);
int64_t cos_audio_close(uint32_t pid);
void    cos_audio_release_for_pid(uint32_t pid);
/* OS-wide master volume (hardware); per-app volume is cos_audio_set_volume(). */
void     cos_audio_master_init(uint32_t vol, bool muted);
uint32_t cos_audio_master_get(void);
bool     cos_audio_master_muted(void);
void     cos_audio_master_set(uint32_t vol);
void     cos_audio_master_toggle_mute(void);
#endif
