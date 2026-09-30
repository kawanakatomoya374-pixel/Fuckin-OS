/**
 * permission_manager.h - Permission Management System
 * C-OS 5.0.0 - UID/GID ベースのアクセス制御
 */

#ifndef PERMISSION_MANAGER_H
#define PERMISSION_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

/* UID/GID 定義 */
#define ROOT_UID 0
#define ROOT_GID 0
#define SYSTEM_UID 1
#define SYSTEM_GID 1

/* パーミッションビット */
#define PERM_OWNER_READ    0400
#define PERM_OWNER_WRITE   0200
#define PERM_OWNER_EXEC    0100
#define PERM_GROUP_READ    0040
#define PERM_GROUP_WRITE   0020
#define PERM_GROUP_EXEC    0010
#define PERM_OTHER_READ    0004
#define PERM_OTHER_WRITE   0002
#define PERM_OTHER_EXEC    0001

/* ユーザー情報 */
typedef struct {
    uint32_t uid;
    uint32_t gid;
    char username[64];
    char password_hash[128];
    uint32_t permissions;
} user_info_t;

/* グループ情報 */
typedef struct {
    uint32_t gid;
    char groupname[64];
    uint32_t member_count;
    uint32_t members[32];
} group_info_t;

/* プロセスコンテキスト */
typedef struct {
    uint32_t uid;
    uint32_t gid;
    uint32_t euid;  /* Effective UID */
    uint32_t egid;  /* Effective GID */
    uint32_t umask; /* Default file creation mask */
} process_context_t;

/* 初期化 */
int permission_manager_init(void);

/* ユーザー管理 */
int permission_create_user(uint32_t uid, const char* username, const char* password);
int permission_delete_user(uint32_t uid);
user_info_t* permission_get_user(uint32_t uid);
user_info_t* permission_find_user_by_name(const char* username);
bool permission_verify_login(const char* username, const char* password);

/* グループ管理 */
int permission_create_group(uint32_t gid, const char* groupname);
int permission_delete_group(uint32_t gid);
group_info_t* permission_get_group(uint32_t gid);
int permission_add_user_to_group(uint32_t uid, uint32_t gid);
int permission_remove_user_from_group(uint32_t uid, uint32_t gid);

/* プロセス権限管理 */
int permission_set_uid(uint32_t uid);
int permission_set_gid(uint32_t gid);
int permission_set_euid(uint32_t euid);
int permission_set_egid(uint32_t egid);
uint32_t permission_get_uid(void);
uint32_t permission_get_gid(void);
uint32_t permission_get_euid(void);
uint32_t permission_get_egid(void);

/* アクセス権限チェック */
int permission_check_access(uint32_t uid, uint32_t gid, uint32_t mode, int flags);
int permission_check_file_access(const char* path, int flags);

/* パーミッション操作 */
int permission_chmod(const char* path, uint32_t mode);
int permission_chown(const char* path, uint32_t uid, uint32_t gid);

#endif /* PERMISSION_MANAGER_H */
