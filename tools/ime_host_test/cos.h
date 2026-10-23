/* Host shim of the few cos.h pieces cosime.c needs, so the IME can be tested on the build machine (tools/ime_host_test/run.sh). */
#ifndef COS_SHIM_H
#define COS_SHIM_H
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#define COS_KEY_NONE 0
#define COS_KEY_ENTER 1
#define COS_KEY_BACKSPACE 2
#define COS_KEY_ESC 3
#define COS_KEY_UP 4
#define COS_KEY_DOWN 5
#define COS_KEY_LEFT 6
#define COS_KEY_RIGHT 7
#define COS_KEY_TAB 8
#define COS_KEY_HOME 10
#define COS_KEY_END 11
#define COS_KEY_F1 16
#define COS_MOD_SHIFT 0x01u
#define COS_MOD_CTRL 0x02u
typedef struct { uint32_t type; int32_t x, y; uint8_t button, buttons, mods, special; int32_t wheel; char ascii; uint8_t pad[3]; } cos_win_event_t;
typedef struct { uint64_t size; uint8_t is_dir; } cos_stat_t;
int cos_stat(const char *path, cos_stat_t *out);
ssize_t cos_read_file(const char *path, void *buf, size_t len);
#endif
