/**
 * storage_manager.h - ストレージ永続化管理
 * C-OS 4.0.8 alpha - VirtualBox Compatible
 */
#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#define STORAGE_MAX_FILES      512
#define STORAGE_MAX_PATH       256
#define STORAGE_MAX_NAME       64
#define STORAGE_MAGIC          0x434F535400000001ULL /* "COST" + version */

/* ファイルエントリ */
typedef struct {
    char name[STORAGE_MAX_NAME];
    char path[STORAGE_MAX_PATH];
    uint64_t size;
    uint64_t created_time;
    uint64_t modified_time;
    uint32_t flags;
    uint8_t type; /* 0=file, 1=directory, 2=symlink */
    uint32_t checksum;
} storage_entry_t;

/* パスワードデータ */
typedef struct {
    char username[32];
    char password_hash[64];
    char salt[32];
    uint64_t last_change;
    uint32_t change_count;
    bool locked;
} password_record_t;

#define MAX_PASSWORDS 16

/* ストレージ状態 */
typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t total_entries;
    uint32_t total_size;
    uint64_t last_save_time;
    uint32_t checksum;
    storage_entry_t entries[STORAGE_MAX_FILES];
    password_record_t passwords[MAX_PASSWORDS];
    uint32_t password_count;
    char metadata[1024];
} storage_state_t;

/* ストレージマネージャー関数 */
int storage_manager_init(void);
int storage_save_state(void);
int storage_load_state(void);
int storage_sync(void);
bool storage_is_dirty(void);

/* ファイル操作
 * 注意: ファイルI/O（storage_read_file / storage_write_file）は
 *       src/drivers/disk/storage.c の VFS ベース実装が正本です。
 *       そちらのシグネチャは (filename, buffer, size) / (filename, buffer, buf_size, *out_size)。
 *       重複リンクエラーを避けるため、ここでは同名シンボルを再宣言しません。 */
int storage_create_file(const char* path, const char* name, uint64_t size);
int storage_delete_file(const char* path);
int storage_list_directory(const char* path, storage_entry_t* entries, int max_count);

/* パスワード管理 */
int storage_set_password(const char* username, const char* password);
int storage_verify_password(const char* username, const char* password);
int storage_change_password(const char* username, const char* old_pass, const char* new_pass);
int storage_get_password_info(const char* username, password_record_t* info);
int storage_lock_account(const char* username);
int storage_unlock_account(const char* username);

/* VirtualBox ストレージ互換 */
int storage_vbox_compat_init(void);
int storage_vbox_save_to_disk(void);
int storage_vbox_load_from_disk(void);

#endif /* STORAGE_MANAGER_H */