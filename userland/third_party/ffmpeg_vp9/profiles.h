/* Stand-in for libavcodec/profiles.h. vp9.c references ff_vp9_profiles
 * only as decorative data inside the inert, unused ff_vp9_decoder
 * FFCodec struct literal at the bottom of the file (see codec_internal.h's
 * file comment - nothing in this port ever looks a codec up by that
 * struct), so a minimal one-entry table is enough for it to compile and
 * link; the content is never read. */
#ifndef COS_FFMPEG_VP9_PROFILES_H
#define COS_FFMPEG_VP9_PROFILES_H

#define FF_PROFILE_UNKNOWN -99

typedef struct AVProfile {
    int profile;
    const char *name;
} AVProfile;

static const AVProfile ff_vp9_profiles[] = {
    { FF_PROFILE_UNKNOWN, "unknown" },
};

extern const AVProfile ff_hevc_profiles[]; /* defined in cos_hevc_shim.c */
#endif
