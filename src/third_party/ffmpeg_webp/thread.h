/* Stand-in for libavcodec/thread.h. Every use it guards in vp8.c/vp8.h
 * is behind "#if HAVE_THREADS" (0 - see config.h), so this only needs
 * to exist; it does not need to declare anything a single-threaded
 * build actually calls. */
#ifndef COS_FFMPEG_WEBP_LAVC_THREAD_H
#define COS_FFMPEG_WEBP_LAVC_THREAD_H
#endif
