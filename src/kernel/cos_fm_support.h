/* cos_fm_support.h - system calls added for ring-3 file managers and other apps that
 * need more than cos_stat()/cos_readdir() give: dates and attributes, whole-directory
 * listing in one call, disk space, "open this in its default app", the desktop's
 * language/theme, and one system-wide clipboard shared by every app.
 *
 * The structs are shared BYTE FOR BYTE with userland/include/cos.h. */
#ifndef COS_FM_SUPPORT_H
#define COS_FM_SUPPORT_H
#include "types.h"

#define COS_ATTR_RDONLY  0x01
#define COS_ATTR_HIDDEN  0x02
#define COS_ATTR_SYSTEM  0x04
#define COS_ATTR_ARCHIVE 0x20

typedef struct { uint64_t size; uint64_t mtime; uint8_t is_dir; uint8_t attr; uint8_t pad[6]; } cos_stat_ex_t;
typedef struct { char name[256]; uint64_t size; uint64_t mtime; uint8_t is_dir; uint8_t attr; uint8_t pad[6]; } cos_dirent_ex_t;

#define COS_LISTDIR_MAX   1024
#define COS_CLIP_MAX      (256u * 1024u)
#define COS_UI_DARK       0x1u
#define COS_UI_JAPANESE   0x2u

bool     cos_fm_stat_ex(const char *path, cos_stat_ex_t *out);
int      cos_fm_listdir_ex(const char *path, cos_dirent_ex_t *out, int max);   /* count, or -1 */
bool     cos_fm_space(uint64_t out[2]);
bool     cos_fm_queue_open(const char *path);
void     cos_fm_pump(void);                       /* GUI thread, once per frame */
uint32_t cos_fm_ui_prefs(void);
int64_t  cos_fm_clip_set(const void *data, uint64_t len);
int64_t  cos_fm_clip_get(void *out, uint64_t cap);   /* length copied, or -1 */

/* single-instance launchers (embedded ring-3 programs) */
void cos_files_open(const char *path);
void cos_files_on_window_created(uint32_t pid);
#endif
