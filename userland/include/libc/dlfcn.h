#ifndef _COS_DLFCN_H
#define _COS_DLFCN_H
/* POSIX dlopen / dlsym / dlclose / dlerror / dladdr over the C-OS loader (cos_dlopen_flags and friends in cos.h).
 *   dlopen runs the library's constructors before it returns; dlclose runs its destructors and really unloads it once nothing uses it.
 *   RTLD_LAZY is accepted and ignored (binding is always eager). */
#include "_cos_libc_cfg.h"
_COS_BEGIN
#define RTLD_LAZY     0x0001
#define RTLD_NOW      0x0002
#define RTLD_NOLOAD   0x0004
#define RTLD_DEEPBIND 0x0008
#define RTLD_GLOBAL   0x0100
#define RTLD_LOCAL    0x0000
#define RTLD_NODELETE 0x1000
#define RTLD_DEFAULT  ((void *)0)
#define RTLD_NEXT     ((void *)-1)
typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } Dl_info;
void *dlopen(const char *path, int flags);
void *dlsym(void *handle, const char *name);
int   dlclose(void *handle);
char *dlerror(void);
int   dladdr(const void *addr, Dl_info *info);
_COS_END
#endif
