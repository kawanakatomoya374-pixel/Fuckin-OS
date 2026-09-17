/**
 * password_screen.c - Password Screen Public API
 * C-OS 4.0.8 alpha
 *
 * NOTE: The actual login UI/logic lives in password_screen_enhanced.c,
 * which performs real password verification via storage_verify_password().
 * This file previously contained a second, unused copy of the login UI
 * whose Enter-key handler set screen_result = 1 (success) unconditionally,
 * without ever checking the entered password. That code was dead (never
 * called from anywhere), but has been removed entirely to avoid any risk
 * of it being wired up later and reintroducing an authentication bypass.
 */
#include "password_screen.h"
#include "storage.h"

/* ============================================================================
   Public API - delegates to the enhanced (verified) implementation
   ========================================================================== */
void password_screen_init(void) {
    password_screen_enhanced_init();
}

bool password_screen_show(void) {
    return password_screen_enhanced_show();
}

void show_password_screen(void) {
    password_screen_show();
}

bool password_screen_loop(void) {
    return password_screen_show();
}

bool password_change_screen_show(void) {
    /* Password change UI is not implemented yet. Return false so callers
     * can fall back to the existing settings flow instead of reporting
     * success for a screen the user never actually saw. */
    return false;
}

bool user_create_account(const char* username, const char* password) {
    (void)username;
    return storage_set_password(password);
}

bool user_verify_login(const char* username, const char* password) {
    (void)username;
    return storage_verify_password(password);
}

bool user_change_password(const char* old_password, const char* new_password) {
    if (storage_verify_password(old_password)) {
        return storage_set_password(new_password);
    }
    return false;
}

bool user_has_account(void) {
    return storage_has_password();
}
