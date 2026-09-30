/* Stand-in for libavutil/log.h. Real FFmpeg's logging is tied to an
 * AVClass on every context for per-component filtering; this port has
 * exactly one "component" (this decoder) so av_log() collapses straight
 * to the kernel's serial console - see cos_ffmpeg_shim.c for the
 * implementation. AVClass stays a forward-declared opaque type: nothing
 * in the ported files dereferences it, only passes pointers around. */
#ifndef COS_FFMPEG_WEBP_LOG_H
#define COS_FFMPEG_WEBP_LOG_H

typedef struct AVClass AVClass;

#define AV_LOG_QUIET   -8
#define AV_LOG_PANIC    0
#define AV_LOG_FATAL    8
#define AV_LOG_ERROR   16
#define AV_LOG_WARNING 24
#define AV_LOG_INFO    32
#define AV_LOG_VERBOSE 40
#define AV_LOG_DEBUG   48
#define AV_LOG_TRACE   56

void av_log(void* avcl, int level, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Logs at initial_level the first time (per *state), subsequent_level
 * every time after - real FFmpeg uses this to warn once instead of
 * spamming every frame. This port's av_log() already goes straight to
 * the serial console with no rate limiting of its own, so this does
 * the *state bookkeeping itself rather than delegating to av_log for
 * the "which level" choice, but still funnels through it either way. */
void av_log_once(void* avcl, int initial_level, int subsequent_level,
                  int* state, const char* fmt, ...)
    __attribute__((format(printf, 5, 6)));

/* Debug/trace/"you hit an unusual but handled case" logging that real
 * FFmpeg compiles out entirely outside a debug build. This port never
 * needs to see them, so they are no-op macros rather than functions -
 * that also makes the variadic-argument type-checking moot (nothing
 * evaluates the arguments). */
#define ff_dlog(ctx, ...) do { } while (0)
#define ff_tlog(ctx, ...) do { } while (0)
#define avpriv_request_sample(ctx, ...) do { } while (0)

#endif
