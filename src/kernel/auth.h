/**
 * auth.h - Authorization & Permission Management
 * 
 * C-OS 4.0.8 alpha 権限管理システム
 */

#ifndef AUTH_H
#define AUTH_H

#include <stdint.h>
#include <stdbool.h>

/* Permission bits */
#define AUTH_READ    (1 << 0)
#define AUTH_WRITE   (1 << 1)
#define AUTH_EXEC    (1 << 2)

/* User roles */
typedef enum {
    ROLE_GUEST,
    ROLE_USER,
    ROLE_ADMIN,
    ROLE_SYSTEM,
} user_role_t;

/* User structure */
typedef struct {
    uint32_t uid;
    uint32_t gid;
    char username[32];
    user_role_t role;
} user_t;

/* Global auth functions */
int auth_init(void);
bool auth_check_file(const char* path, uint32_t uid, uint8_t required_perms);
bool auth_is_admin(uint32_t uid);
user_t* auth_get_current_user(void);

#endif
