/**
 * permission_manager.c - Permission Management System Implementation
 * C-OS 5.0.0 - UID/GID ベースのアクセス制御
 */

#include "permission_manager.h"
#include "../include/serial.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../system/storage_manager.h"
#include "../../fs/fs_unified.h"
#include "../include/task.h"
#include "../include/sync.h"

#define MAX_USERS 256
#define MAX_GROUPS 64

static user_info_t g_users[MAX_USERS];
static group_info_t g_groups[MAX_GROUPS];
static process_context_t g_current_context;
static int g_user_count = 0;
static int g_group_count = 0;
static bool g_initialized = false;

#define PERM_CRIT_BEGIN() uint64_t __perm_flags = sync_irq_save()
#define PERM_CRIT_END()   sync_irq_restore(__perm_flags)

static process_t* permission_current_process(void) {
    return process_get_current();
}

static bool permission_is_privileged(void) {
    process_t* proc = permission_current_process();
    if (proc) {
        return proc->euid == ROOT_UID;
    }
    return g_current_context.euid == ROOT_UID;
}

static void permission_read_context(process_context_t* out) {
    if (!out) return;
    process_t* proc = permission_current_process();
    if (proc) {
        out->uid = proc->uid;
        out->gid = proc->gid;
        out->euid = proc->euid;
        out->egid = proc->egid;
        out->umask = proc->umask;
        return;
    }
    *out = g_current_context;
}

static void permission_write_context(const process_context_t* in) {
    if (!in) return;
    process_t* proc = permission_current_process();
    if (proc) {
        proc->uid = in->uid;
        proc->gid = in->gid;
        proc->euid = in->euid;
        proc->egid = in->egid;
        proc->umask = in->umask;
        return;
    }
    g_current_context = *in;
}

/* Forward declarations for cross-calling management helpers. */
int permission_create_group(uint32_t gid, const char* groupname);
group_info_t* permission_get_group(uint32_t gid);
int permission_add_user_to_group(uint32_t uid, uint32_t gid);

static bool permission_user_in_group(uint32_t uid, uint32_t gid) {
    for (int i = 0; i < g_group_count; ++i) {
        if (g_groups[i].gid != gid) continue;
        for (uint32_t j = 0; j < g_groups[i].member_count; ++j) {
            if (g_groups[i].members[j] == uid) {
                return true;
            }
        }
    }
    return false;
}

static bool permission_copy_name(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0 || !src) return false;
    size_t len = strlen(src);
    if (len >= dst_size) return false;
    memcpy(dst, src, len + 1);
    return true;
}

static void permission_remove_user_from_all_groups(uint32_t uid) {
    for (int i = 0; i < g_group_count; ++i) {
        group_info_t* group = &g_groups[i];
        for (uint32_t j = 0; j < group->member_count; ++j) {
            if (group->members[j] == uid) {
                group->members[j] = group->members[group->member_count - 1];
                if (group->member_count > 0) {
                    group->member_count--;
                }
                --j;
            }
        }
    }
}

/* ============================================================
 * 初期化
 * ============================================================ */

int permission_manager_init(void) {
    if (g_initialized) return 0;
    
    serial_puts("[PERM] Initializing permission manager...\n");
    
    memset(g_users, 0, sizeof(g_users));
    memset(g_groups, 0, sizeof(g_groups));
    memset(&g_current_context, 0, sizeof(g_current_context));
    
    /* デフォルトユーザー作成 */
    g_users[0].uid = ROOT_UID;
    if (!permission_copy_name(g_users[0].username, sizeof(g_users[0].username), "root")) return -1;
    // パスワードはstorage.cで管理されるため、ここでは設定しない
    g_users[0].permissions = 0777;
    g_user_count = 1;
    
    /* デフォルトグループ作成 */
    g_groups[0].gid = ROOT_GID;
    if (!permission_copy_name(g_groups[0].groupname, sizeof(g_groups[0].groupname), "root")) return -1;
    g_groups[0].member_count = 1;
    g_groups[0].members[0] = ROOT_UID;
    g_group_count = 1;
    
    /* 現在のコンテキストをrootに設定 */
    g_current_context.uid = ROOT_UID;
    g_current_context.gid = ROOT_GID;
    g_current_context.euid = ROOT_UID;
    g_current_context.egid = ROOT_GID;
    g_current_context.umask = 0022;

    permission_write_context(&g_current_context);
    
    g_initialized = true;
    serial_puts("[PERM] Permission manager initialized\n");
    return 0;
}

/* ============================================================
 * ユーザー管理
 * ============================================================ */

int permission_create_user(uint32_t uid, const char* username, const char* password) {
    if (!g_initialized || g_user_count >= MAX_USERS) return -1;
    if (!permission_is_privileged()) return -1;
    if (!username) return -1;
    PERM_CRIT_BEGIN();
    (void)password;

    /* UID が既に存在するかチェック */
    for (int i = 0; i < g_user_count; i++) {
        if (g_users[i].uid == uid) {
            PERM_CRIT_END();
            return -1;
        }
    }

    bool created_group = false;
    /* Each user gets a private primary group by default so new accounts
     * do not inherit root-group privileges. */
    if (!permission_get_group(uid) && g_group_count < MAX_GROUPS) {
        if (permission_create_group(uid, username) != 0) {
            PERM_CRIT_END();
            return -1;
        }
        created_group = true;
    }
    
    user_info_t* user = &g_users[g_user_count++];
    memset(user, 0, sizeof(*user));
    user->uid = uid;
    user->gid = uid;
    if (!permission_copy_name(user->username, sizeof(user->username), username)) {
        g_user_count--;
        if (created_group) {
            permission_delete_group(uid);
        }
        PERM_CRIT_END();
        return -1;
    }
    /* Add the user to its primary group if that group exists. */
    if (permission_get_group(uid)) {
        if (permission_add_user_to_group(uid, uid) != 0) {
            g_user_count--;
            if (created_group) {
                permission_delete_group(uid);
            }
            memset(user, 0, sizeof(*user));
            PERM_CRIT_END();
            return -1;
        }
    }
    // パスワードはstorage.cで管理されるため、ここでは設定しない
    user->permissions = 0755;
    PERM_CRIT_END();
    return 0;
}

int permission_delete_user(uint32_t uid) {
    if (!g_initialized || uid == ROOT_UID) return -1;
    if (!permission_is_privileged()) return -1;
    PERM_CRIT_BEGIN();
    
    for (int i = 0; i < g_user_count; i++) {
        if (g_users[i].uid == uid) {
            permission_remove_user_from_all_groups(uid);
            for (process_t* proc = task_get_first(); proc; proc = task_get_next(proc)) {
                if (proc->state == TASK_UNUSED || proc->state == TASK_ZOMBIE) continue;
                if (proc->uid == uid) proc->uid = ROOT_UID;
                if (proc->euid == uid) proc->euid = ROOT_UID;
            }
            if (g_current_context.uid == uid) {
                g_current_context.uid = ROOT_UID;
            }
            if (g_current_context.euid == uid) {
                g_current_context.euid = ROOT_UID;
            }
            /* ユーザーを削除（最後のユーザーと入れ替え） */
            g_users[i] = g_users[g_user_count - 1];
            g_user_count--;
            PERM_CRIT_END();
            return 0;
        }
    }
    PERM_CRIT_END();
    return -1;
}

user_info_t* permission_get_user(uint32_t uid) {
    if (!g_initialized) return NULL;
    PERM_CRIT_BEGIN();
    for (int i = 0; i < g_user_count; i++) {
        if (g_users[i].uid == uid) {
            PERM_CRIT_END();
            return &g_users[i];
        }
    }
    PERM_CRIT_END();
    return NULL;
}

user_info_t* permission_find_user_by_name(const char* username) {
    if (!g_initialized || !username) return NULL;
    PERM_CRIT_BEGIN();
    for (int i = 0; i < g_user_count; i++) {
        if (strcmp(g_users[i].username, username) == 0) {
            PERM_CRIT_END();
            return &g_users[i];
        }
    }
    PERM_CRIT_END();
    return NULL;
}

/* ============================================================
 * グループ管理
 * ============================================================ */

int permission_create_group(uint32_t gid, const char* groupname) {
    if (!g_initialized || g_group_count >= MAX_GROUPS) return -1;
    if (!permission_is_privileged()) return -1;
    if (!groupname) return -1;
    PERM_CRIT_BEGIN();
    
    /* GID が既に存在するかチェック */
    for (int i = 0; i < g_group_count; i++) {
        if (g_groups[i].gid == gid) {
            PERM_CRIT_END();
            return -1;
        }
    }

    group_info_t* group = &g_groups[g_group_count++];
    group->gid = gid;
    if (!permission_copy_name(group->groupname, sizeof(group->groupname), groupname)) {
        g_group_count--;
        PERM_CRIT_END();
        return -1;
    }
    group->member_count = 0;
    PERM_CRIT_END();
    return 0;
}

int permission_delete_group(uint32_t gid) {
    if (!g_initialized || gid == ROOT_GID) return -1;
    if (!permission_is_privileged()) return -1;
    PERM_CRIT_BEGIN();
    
    for (int i = 0; i < g_group_count; i++) {
        if (g_groups[i].gid == gid) {
            for (int u = 0; u < g_user_count; ++u) {
                if (g_users[u].gid == gid) {
                    g_users[u].gid = ROOT_GID;
                }
            }
            for (process_t* proc = task_get_first(); proc; proc = task_get_next(proc)) {
                if (proc->state == TASK_UNUSED || proc->state == TASK_ZOMBIE) continue;
                if (proc->gid == gid) proc->gid = ROOT_GID;
                if (proc->egid == gid) proc->egid = ROOT_GID;
            }
            if (g_current_context.gid == gid) {
                g_current_context.gid = ROOT_GID;
            }
            if (g_current_context.egid == gid) {
                g_current_context.egid = ROOT_GID;
            }
            g_groups[i] = g_groups[g_group_count - 1];
            g_group_count--;
            PERM_CRIT_END();
            return 0;
        }
    }
    PERM_CRIT_END();
    return -1;
}

group_info_t* permission_get_group(uint32_t gid) {
    if (!g_initialized) return NULL;
    PERM_CRIT_BEGIN();
    for (int i = 0; i < g_group_count; i++) {
        if (g_groups[i].gid == gid) {
            PERM_CRIT_END();
            return &g_groups[i];
        }
    }
    PERM_CRIT_END();
    return NULL;
}

int permission_add_user_to_group(uint32_t uid, uint32_t gid) {
    if (!g_initialized) return -1;
    if (!permission_is_privileged()) return -1;
    if (permission_get_user(uid) == NULL) return -1;
    PERM_CRIT_BEGIN();
    
    group_info_t* group = permission_get_group(gid);
    if (!group || group->member_count >= 32) {
        PERM_CRIT_END();
        return -1;
    }
    
    /* 既に所属しているかチェック */
    for (int i = 0; i < (int)group->member_count; i++) {
        if (group->members[i] == uid) {
            PERM_CRIT_END();
            return 0;
        }
    }
    
    group->members[group->member_count++] = uid;
    PERM_CRIT_END();
    return 0;
}

int permission_remove_user_from_group(uint32_t uid, uint32_t gid) {
    if (!g_initialized) return -1;
    if (!permission_is_privileged()) return -1;
    PERM_CRIT_BEGIN();
    
    group_info_t* group = permission_get_group(gid);
    if (!group) {
        PERM_CRIT_END();
        return -1;
    }
    
    for (int i = 0; i < (int)group->member_count; i++) {
        if (group->members[i] == uid) {
            group->members[i] = group->members[group->member_count - 1];
            group->member_count--;
            PERM_CRIT_END();
            return 0;
        }
    }
    PERM_CRIT_END();
    return -1;
}

/* ============================================================
 * プロセス権限管理
 * ============================================================ */

int permission_set_uid(uint32_t uid) {
    if (!g_initialized) return -1;
    if (!permission_is_privileged()) return -1;
    if (permission_get_user(uid) == NULL) return -1;
    PERM_CRIT_BEGIN();
    process_context_t ctx;
    permission_read_context(&ctx);
    ctx.uid = uid;
    permission_write_context(&ctx);
    PERM_CRIT_END();
    return 0;
}

int permission_set_gid(uint32_t gid) {
    if (!g_initialized) return -1;
    if (!permission_is_privileged()) return -1;
    if (permission_get_group(gid) == NULL) return -1;
    PERM_CRIT_BEGIN();
    process_context_t ctx;
    permission_read_context(&ctx);
    ctx.gid = gid;
    permission_write_context(&ctx);
    PERM_CRIT_END();
    return 0;
}

int permission_set_euid(uint32_t euid) {
    if (!g_initialized) return -1;
    if (!permission_is_privileged()) return -1;
    if (permission_get_user(euid) == NULL) return -1;
    PERM_CRIT_BEGIN();
    process_context_t ctx;
    permission_read_context(&ctx);
    ctx.euid = euid;
    permission_write_context(&ctx);
    PERM_CRIT_END();
    return 0;
}

int permission_set_egid(uint32_t egid) {
    if (!g_initialized) return -1;
    if (!permission_is_privileged()) return -1;
    if (permission_get_group(egid) == NULL) return -1;
    PERM_CRIT_BEGIN();
    process_context_t ctx;
    permission_read_context(&ctx);
    ctx.egid = egid;
    permission_write_context(&ctx);
    PERM_CRIT_END();
    return 0;
}

uint32_t permission_get_uid(void) {
    PERM_CRIT_BEGIN();
    process_context_t ctx;
    permission_read_context(&ctx);
    PERM_CRIT_END();
    return ctx.uid;
}

uint32_t permission_get_gid(void) {
    PERM_CRIT_BEGIN();
    process_context_t ctx;
    permission_read_context(&ctx);
    PERM_CRIT_END();
    return ctx.gid;
}

uint32_t permission_get_euid(void) {
    PERM_CRIT_BEGIN();
    process_context_t ctx;
    permission_read_context(&ctx);
    PERM_CRIT_END();
    return ctx.euid;
}

uint32_t permission_get_egid(void) {
    PERM_CRIT_BEGIN();
    process_context_t ctx;
    permission_read_context(&ctx);
    PERM_CRIT_END();
    return ctx.egid;
}

/* ============================================================
 * アクセス権限チェック
 * ============================================================ */

int permission_check_access(uint32_t uid, uint32_t gid, uint32_t mode, int flags) {
    if (!g_initialized) return -1;
    PERM_CRIT_BEGIN();

    process_context_t ctx;
    permission_read_context(&ctx);

    /* Root は常にアクセス可能 */
    if (ctx.euid == ROOT_UID) {
        PERM_CRIT_END();
        return 0;
    }

    int can_read = 0, can_write = 0, can_exec = 0;

    /* 所有者のパーミッションをチェック */
    if (ctx.euid == uid || ctx.uid == uid) {
        can_read = (mode & PERM_OWNER_READ) != 0;
        can_write = (mode & PERM_OWNER_WRITE) != 0;
        can_exec = (mode & PERM_OWNER_EXEC) != 0;
    }
    /* グループのパーミッションをチェック */
    else if (ctx.egid == gid || permission_user_in_group(ctx.uid, gid)) {
        can_read = (mode & PERM_GROUP_READ) != 0;
        can_write = (mode & PERM_GROUP_WRITE) != 0;
        can_exec = (mode & PERM_GROUP_EXEC) != 0;
    }
    /* その他のパーミッションをチェック */
    else {
        can_read = (mode & PERM_OTHER_READ) != 0;
        can_write = (mode & PERM_OTHER_WRITE) != 0;
        can_exec = (mode & PERM_OTHER_EXEC) != 0;
    }

    if ((flags & 0001) && !can_read) return -1;
    if ((flags & 0002) && !can_write) return -1;
    if ((flags & 0100) && !can_exec) {
        PERM_CRIT_END();
        return -1;
    }

    PERM_CRIT_END();
    return 0;
}

int permission_check_file_access(const char* path, int flags) {
    if (!g_initialized || !path) return -1;
    PERM_CRIT_BEGIN();

    fs_unified_stat_t st;
    if (fs_unified_stat(path, &st) != 0) {
        PERM_CRIT_END();
        return -1;
    }

    int rc = permission_check_access(st.uid, st.gid, st.mode, flags);
    PERM_CRIT_END();
    return rc;
}

/* ============================================================
 * パーミッション操作
 * ============================================================ */

int permission_chmod(const char* path, uint32_t mode) {
    if (!g_initialized || !path) return -1;

    /* Root のみが chmod 可能 */
    if (permission_get_euid() != ROOT_UID) return -1;

    PERM_CRIT_BEGIN();
    int rc = fs_unified_chmod(path, mode);
    PERM_CRIT_END();
    return rc;
}

int permission_chown(const char* path, uint32_t uid, uint32_t gid) {
    if (!g_initialized || !path) return -1;

    /* Root のみが chown 可能 */
    if (permission_get_euid() != ROOT_UID) return -1;

    PERM_CRIT_BEGIN();
    int rc = fs_unified_chown(path, uid, gid);
    PERM_CRIT_END();
    return rc;
}

bool permission_verify_login(const char* username, const char* password) {
    if (!g_initialized || !username || !password) return false;
    PERM_CRIT_BEGIN();
    bool ok = storage_verify_password(username, password) == 0;
    PERM_CRIT_END();
    return ok;
}
