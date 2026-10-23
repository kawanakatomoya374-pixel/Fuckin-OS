#ifndef PASSWORD_SCREEN_H
#define PASSWORD_SCREEN_H

#include "types.h"

bool password_screen_show(void);
void show_password_screen(void);

bool user_create_account(const char* username, const char* password);
bool user_change_password(const char* old_password, const char* new_password);
bool user_has_account(void);

void password_screen_enhanced_init(void);
bool password_screen_enhanced_show(void);
#endif
