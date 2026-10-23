/* Stand-in for libavutil/error.h. Real FFmpeg picks negative errno-space
 * values that don't collide with platform errno on any of its supported
 * OSes; this port only ever inspects these values for < 0 / == a specific
 * constant within the ported files themselves (nothing here maps them
 * back to strerror or crosses a real syscall boundary), so a plain
 * negated-errno scheme is sufficient. */
#ifndef COS_FFMPEG_WEBP_ERROR_H
#define COS_FFMPEG_WEBP_ERROR_H

#include <errno.h>

#define AVERROR(e) (-(e))
#define AVERROR_EOF        (-('E'+('O'<<8)+('F'<<16)+('!'<<24)))
#define AVERROR_BUG        (-('B'+('U'<<8)+('G'<<16)+('!'<<24)))
#define AVERROR_INVALIDDATA (-('I'+('N'<<8)+('D'<<16)+('A'<<24)))
#define AVERROR_PATCHWELCOME (-('P'+('A'<<8)+('W'<<16)+('E'<<24)))
#define AVERROR_UNKNOWN    (-('U'+('N'<<8)+('K'<<16)+('!'<<24)))

#endif
