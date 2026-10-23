/* Stand-in for libavutil/thread.h with HAVE_THREADS==0 (see config.h) -
 * this mirrors the shape of FFmpeg's own !HAVE_THREADS fallback in that
 * same file: the mutex/cond types exist purely so struct fields that
 * mention them (e.g. VP8ThreadData in vp8.h) still have a type, and
 * every operation on them is a no-op, since this port only ever decodes
 * on whichever single thread called it - see progressframe.h in this
 * directory for the piece that would otherwise need them to do real
 * work. */
#ifndef COS_FFMPEG_WEBP_THREAD_H
#define COS_FFMPEG_WEBP_THREAD_H

/* __GLIBC__ is never defined in the real (freestanding, no-libc) kernel
 * build this port targets - it only ever shows up here because of the
 * developer-machine host-mode test harness (cos_webp_host_test.c),
 * which links against the real system libc/pthread headers purely to
 * get fast pre-integration decode-correctness feedback (see that
 * file's own comment) and therefore already has real pthread_mutex_t/
 * pthread_cond_t/pthread_t from <sys/types.h> - defining these again
 * with different (int) underlying types would conflict. In the actual
 * kernel target there is nothing to conflict with, so the plain (int)
 * stand-ins are used, same as any other !HAVE_THREADS build. */
#ifndef __GLIBC__
typedef int pthread_mutex_t;
typedef int pthread_cond_t;
typedef int pthread_t;
#endif

#define pthread_mutex_init(m, a)    (0)
#define pthread_mutex_destroy(m)    (0)
#define pthread_mutex_lock(m)       (0)
#define pthread_mutex_unlock(m)     (0)
#define pthread_cond_init(c, a)     (0)
#define pthread_cond_destroy(c)     (0)
#define pthread_cond_signal(c)      (0)
#define pthread_cond_broadcast(c)   (0)
#define pthread_cond_wait(c, m)     (0)

#define AVOnce int
#define AV_ONCE_INIT 0
#define ff_thread_once(o, f) do { if (!*(o)) { f(); *(o) = 1; } } while (0)

#endif
