#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#include "py/builtin.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "py/mperrno.h"
#include "../../include/cos_api.h"
#include "../../fs/fs.h"

extern const char *fs_read_file_at(const char *path, const char *name);

#if !MICROPY_VFS

#ifndef S_IFDIR
#define S_IFDIR 0040000
#endif

#ifndef MP_OBJ_NULL
#define MP_OBJ_NULL MP_OBJ_FROM_PTR(NULL)
#endif

#define COS_PY_MAX_PATH      256
#define COS_PY_MAX_FILE      FS_MAX_DATA
#define COS_PY_MAX_STAGING    16

typedef struct _mp_obj_cos_file_t {
    mp_obj_base_t base;
    bool readable;
    bool writable;
    bool append;
    bool dirty;
    char path[COS_PY_MAX_PATH];
    size_t pos;
    size_t size;
    char data[COS_PY_MAX_FILE + 1];
} mp_obj_cos_file_t;

static const char *cos_normalize_path(const char *path, char *buf, size_t buf_len) {
    if (path == NULL || path[0] == '\0') {
        return NULL;
    }
    if (path[0] == '/') {
        return path;
    }
    if (snprintf(buf, buf_len, "/%s", path) < 0) {
        return NULL;
    }
    return buf;
}

static void cos_file_sync(mp_obj_cos_file_t *self) {
    if (!self || !self->writable || !self->dirty) {
        return;
    }
    if (self->size > COS_PY_MAX_FILE) {
        self->size = COS_PY_MAX_FILE;
    }
    self->data[self->size] = '\0';
    (void)cos_fs_write_file(self->path, self->data, (uint64_t)self->size);
    self->dirty = false;
}

static mp_import_stat_t mp_import_stat_impl(const char *path) {
    char norm_path[COS_PY_MAX_PATH];
    const char *resolved = cos_normalize_path(path, norm_path, sizeof(norm_path));
    if (resolved == NULL) {
        return MP_IMPORT_STAT_NO_EXIST;
    }
    char parent[COS_PY_MAX_PATH];
    char leaf[COS_PY_MAX_PATH];
    const char *last_slash = strrchr(resolved, '/');
    if (!last_slash) {
        strcpy(parent, "/");
        strncpy(leaf, resolved, sizeof(leaf) - 1);
        leaf[sizeof(leaf) - 1] = '\0';
    } else if (last_slash == resolved) {
        strcpy(parent, "/");
        strncpy(leaf, resolved + 1, sizeof(leaf) - 1);
        leaf[sizeof(leaf) - 1] = '\0';
    } else {
        size_t parent_len = (size_t)(last_slash - resolved);
        if (parent_len >= sizeof(parent)) parent_len = sizeof(parent) - 1;
        memcpy(parent, resolved, parent_len);
        parent[parent_len] = '\0';
        strncpy(leaf, last_slash + 1, sizeof(leaf) - 1);
        leaf[sizeof(leaf) - 1] = '\0';
    }
    if (leaf[0] == '\0') {
        return MP_IMPORT_STAT_NO_EXIST;
    }
    const char *data = fs_read_file_at(parent, leaf);
    if (data == NULL) {
        return MP_IMPORT_STAT_NO_EXIST;
    }
    size_t size = strlen(data);
    if (size > 0) {
        return MP_IMPORT_STAT_FILE;
    }
    /* empty files are still valid import targets for stat purposes */
    return MP_IMPORT_STAT_FILE;
}

mp_import_stat_t mp_import_stat(const char *path) {
    return mp_import_stat_impl(path);
}

static void cos_file_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    const mp_obj_cos_file_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<C-OS file path=%s pos=%u size=%u>", self->path, (unsigned)self->pos, (unsigned)self->size);
}

static void cos_file_close(mp_obj_cos_file_t *self) {
    if (!self) {
        return;
    }
    cos_file_sync(self);
}

static mp_uint_t cos_file_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode) {
    mp_obj_cos_file_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->readable) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    if (self->pos >= self->size) {
        return 0;
    }
    size_t avail = self->size - self->pos;
    if ((size_t)size > avail) size = (mp_uint_t)avail;
    memcpy(buf, self->data + self->pos, (size_t)size);
    self->pos += (size_t)size;
    return size;
}

static mp_uint_t cos_file_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode) {
    mp_obj_cos_file_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->writable) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    if (self->append) {
        self->pos = self->size;
    }
    if (self->pos >= COS_PY_MAX_FILE) {
        *errcode = MP_EFBIG;
        return MP_STREAM_ERROR;
    }
    size_t room = COS_PY_MAX_FILE - self->pos;
    if ((size_t)size > room) size = (mp_uint_t)room;
    memcpy(self->data + self->pos, buf, (size_t)size);
    self->pos += (size_t)size;
    if (self->pos > self->size) self->size = self->pos;
    self->data[self->size] = '\0';
    self->dirty = true;
    return size;
}

static mp_uint_t cos_file_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    mp_obj_cos_file_t *self = MP_OBJ_TO_PTR(self_in);
    switch (request) {
        case MP_STREAM_FLUSH:
            cos_file_sync(self);
            return 0;
        case MP_STREAM_CLOSE:
            cos_file_close(self);
            return 0;
        case MP_STREAM_SEEK: {
            struct mp_stream_seek_t *s = (struct mp_stream_seek_t *)(uintptr_t)arg;
            size_t base = 0;
            switch (s->whence) {
                case SEEK_SET: base = 0; break;
                case SEEK_CUR: base = self->pos; break;
                case SEEK_END: base = self->size; break;
                default:
                    *errcode = MP_EINVAL;
                    return MP_STREAM_ERROR;
            }
            long new_pos = (long)base + (long)s->offset;
            if (new_pos < 0) {
                *errcode = MP_EINVAL;
                return MP_STREAM_ERROR;
            }
            self->pos = (size_t)new_pos;
            s->offset = (off_t)self->pos;
            return 0;
        }
        case MP_STREAM_GET_FILENO:
            return 0;
        case MP_STREAM_GET_BUFFER_SIZE:
            return 256;
        default:
            *errcode = MP_EINVAL;
            return MP_STREAM_ERROR;
    }
}

static const mp_stream_p_t cos_file_stream_p = {
    .read = cos_file_read,
    .write = cos_file_write,
    .ioctl = cos_file_ioctl,
    .is_text = 0,
};

static MP_DEFINE_CONST_OBJ_TYPE(
    mp_type_cos_file,
    MP_QSTR_io,
    MP_TYPE_FLAG_NONE,
    print, cos_file_print,
    protocol, &cos_file_stream_p
    );

static int cos_open_flags_from_mode(const char *mode, bool *readable, bool *writable, bool *append, bool *truncate, bool *create, bool *exclusive) {
    if (mode == NULL || mode[0] == '\0') {
        return -1;
    }
    *readable = false;
    *writable = false;
    *append = false;
    *truncate = false;
    *create = false;
    *exclusive = false;
    switch (mode[0]) {
        case 'r': *readable = true; break;
        case 'w': *writable = true; *truncate = true; *create = true; break;
        case 'a': *writable = true; *append = true; *create = true; break;
        case 'x': *writable = true; *create = true; *exclusive = true; break;
        default: return -1;
    }
    for (const char *p = mode + 1; *p; ++p) {
        if (*p == '+') {
            *readable = true;
            *writable = true;
        }
    }
    if (!*readable && !*writable) {
        return -1;
    }
    return 0;
}

static int cos_file_load(mp_obj_cos_file_t *o, const char *path, const char *mode) {
    bool readable, writable, append, truncate, create, exclusive;
    if (cos_open_flags_from_mode(mode, &readable, &writable, &append, &truncate, &create, &exclusive) != 0) {
        return -1;
    }

    memset(o, 0, sizeof(*o));
    o->base.type = &mp_type_cos_file;
    o->readable = readable;
    o->writable = writable;
    o->append = append;
    o->dirty = false;
    o->pos = 0;
    o->size = 0;
    strncpy(o->path, path, sizeof(o->path) - 1);
    o->path[sizeof(o->path) - 1] = '\0';

    if (truncate) {
        o->dirty = true;
        o->data[0] = '\0';
        o->size = 0;
        o->pos = 0;
        return 0;
    }

    char parent[COS_PY_MAX_PATH];
    char leaf[COS_PY_MAX_PATH];
    const char *last_slash = strrchr(path, '/');
    if (!last_slash) {
        strcpy(parent, "/");
        strncpy(leaf, path, sizeof(leaf) - 1);
        leaf[sizeof(leaf) - 1] = '\0';
    } else if (last_slash == path) {
        strcpy(parent, "/");
        strncpy(leaf, path + 1, sizeof(leaf) - 1);
        leaf[sizeof(leaf) - 1] = '\0';
    } else {
        size_t parent_len = (size_t)(last_slash - path);
        if (parent_len >= sizeof(parent)) parent_len = sizeof(parent) - 1;
        memcpy(parent, path, parent_len);
        parent[parent_len] = '\0';
        strncpy(leaf, last_slash + 1, sizeof(leaf) - 1);
        leaf[sizeof(leaf) - 1] = '\0';
    }

    if (leaf[0] != '\0') {
        const char *src = fs_read_file_at(parent, leaf);
        if (src) {
            size_t len = strlen(src);
            if (len > COS_PY_MAX_FILE) len = COS_PY_MAX_FILE;
            memcpy(o->data, src, len);
            o->data[len] = '\0';
            o->size = len;
            if (append) {
                o->pos = o->size;
            }
            return 0;
        }
    }

    if (create) {
        o->data[0] = '\0';
        o->size = 0;
        o->pos = append ? o->size : 0;
        o->dirty = true;
        return 0;
    }

    return -1;
}

mp_obj_t mp_builtin_open(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    (void)kwargs;
    if (n_args < 1 || n_args > 2) {
        mp_raise_TypeError(MP_ERROR_TEXT("open() expects path and optional mode"));
    }

    const char *path = mp_obj_str_get_str(args[0]);
    const char *mode = (n_args >= 2) ? mp_obj_str_get_str(args[1]) : "r";

    char norm_path[COS_PY_MAX_PATH];
    const char *resolved = cos_normalize_path(path, norm_path, sizeof(norm_path));
    if (resolved == NULL) {
        mp_raise_OSError(MP_ENOENT);
    }

    mp_obj_cos_file_t *o = mp_obj_malloc(mp_obj_cos_file_t, &mp_type_cos_file);
    if (cos_file_load(o, resolved, mode) != 0) {
        mp_raise_OSError(MP_ENOENT);
    }
    return MP_OBJ_FROM_PTR(o);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, mp_builtin_open);

#endif // !MICROPY_VFS
