/**
 * progressframe.h - stand-in for libavcodec/progressframe.h.
 *
 * Real FFmpeg's ProgressFrame lets one thread decode macroblock row N of a
 * frame while another thread that only needs rows up to N-1 (e.g. motion
 * compensation reading a reference frame) proceeds without waiting for the
 * whole frame - ff_progress_frame_await() blocks until a given row number
 * has been ff_progress_frame_report()'d by the decoding thread.
 *
 * This port is single-threaded (HAVE_THREADS is 0 in config.h - see its
 * comment for why): there is never a second thread that could be
 * "waiting" on a row, because nothing else runs concurrently with the one
 * call into this decoder. So ff_progress_frame_report()/_await() below
 * are correct (not merely simplified) no-ops for this build - by the
 * time any code could call _await(), the only thread that could ever
 * report progress has either already finished the whole frame or has not
 * been entered yet, so "wait for row N" and "row N is already done, or
 * this frame decode has not started" are the only two cases that can
 * actually occur, and both mean there is nothing to block on.
 *
 * Implementation of the functions declared here is in cos_ffmpeg_shim.c.
 */
#ifndef COS_FFMPEG_WEBP_PROGRESSFRAME_H
#define COS_FFMPEG_WEBP_PROGRESSFRAME_H

#include "avcodec.h"

#define AV_GET_BUFFER_FLAG_REF 1

typedef struct ProgressFrame {
    AVFrame* f;
} ProgressFrame;

int  ff_progress_frame_get_buffer(AVCodecContext* avctx, ProgressFrame* pf, int flags);
void ff_progress_frame_unref(ProgressFrame* pf);
void ff_progress_frame_replace(ProgressFrame* dst, const ProgressFrame* src);
void ff_progress_frame_report(ProgressFrame* pf, int n);
void ff_progress_frame_await(const ProgressFrame* pf, int n);

#endif
