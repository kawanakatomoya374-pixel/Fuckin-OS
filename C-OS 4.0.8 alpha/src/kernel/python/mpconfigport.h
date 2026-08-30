/**
 * mpconfigport.h - MicroPython Port Configuration for C-OS 4.0.8 alpha
 */
#include <stdint.h>

/* OS specific configuration */
#define MICROPY_PY_SYS_PLATFORM             "c-os"

/* Features */
#define MICROPY_ENABLE_GC                   (1)
#define MICROPY_HELPER_REPL                 (1)
#define MICROPY_PY_BUILTINS_FLOAT           (1)
#define MICROPY_PY_BUILTINS_COMPLEX         (0)
#define MICROPY_PY_IO                       (1)
#define MICROPY_PY_SYS                      (1)
#define MICROPY_PY_SYS_STDFILES             (1)
#define MICROPY_PY_SYS_EXIT                 (1)
#define MICROPY_PY_STRUCT                   (1)
#define MICROPY_PY_UBINASCII                (1)
#define MICROPY_PY_UCTYPES                  (1)
#define MICROPY_PY_UERRNO                   (1)
#define MICROPY_PY_UJSON                    (1)
#define MICROPY_PY_URE                      (1)
#define MICROPY_PY_USELECT                  (0)
#define MICROPY_PY_UTIME                    (1)
#define MICROPY_PY_UTIMEQ                   (0)
#define MICROPY_PY_UZLIB                    (0)

/* Memory management */
#define MICROPY_ALLOC_PATH_MAX              (256)
#define MICROPY_ALLOC_PARSE_CHUNK_INIT      (16)

/* Type definitions */
typedef intptr_t mp_int_t;
typedef uintptr_t mp_uint_t;
typedef long mp_off_t;

/* Root pointers */
#define MICROPY_PORT_ROOT_POINTERS \
    const char *readline_hist[8];

#define MICROPY_VFS (0)
#define MICROPY_READER_VFS (0)
