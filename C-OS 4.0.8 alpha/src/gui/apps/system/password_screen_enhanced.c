/**
 * password_screen_enhanced.c - Enhanced Password Screen with Modern UI
 * C-OS 4.0.8 alpha - Redesigned Login Interface
 * 
 * Features:
 * - Modern flat design with rounded corners
 * - Semi-transparent backgrounds
 * - User avatar display
 * - Smooth animations (simulated)
 * - Improved error messages
 * - Japanese language support
 */
#include "password_screen.h"
#include "storage.h"
#include "../../../kernel/security/permission_manager.h"
#include "storage.h"
#include "gui.h"
#include "timer.h"
#include "vga.h"
#include "serial.h"
#include "../../drivers/input/keyboard.h"
#include "../../drivers/input/mouse_minimal.h"
#include "rtc.h"
#include "string.h"
#include <stdint.h>

/* ============================================================================
   Configuration
   ========================================================================== */
#define PW_SCREEN_W ((int)SCREEN_W)
#define PW_SCREEN_H ((int)SCREEN_H)
#define PASS_MAX_LEN 32
#define BOOT_PASSWORD_TIMEOUT_TICKS 10000u

/* Color Scheme - matches C-OS boot animation (navy / cyan) instead of purple */
#define COLOR_BG_DARK 0x00101A30
#define COLOR_CARD_BG 0x0014223C
#define COLOR_CARD_BORDER 0x006088BE
#define COLOR_INPUT_BG 0x00121C2C
#define COLOR_INPUT_BORDER 0x00466EA5
#define COLOR_INPUT_FOCUS 0x00589CEE
#define COLOR_BUTTON_BG 0x00466EA5
#define COLOR_BUTTON_HOVER 0x00589CEE
#define COLOR_BUTTON_ACTIVE 0x006EA8F6
#define COLOR_TEXT_PRIMARY 0x00ECF6FF
#define COLOR_TEXT_SECONDARY 0x009ECEFF
#define COLOR_ERROR 0xFF6B6B
#define COLOR_SUCCESS 0x51CF66

/* UI Dimensions */
#define CARD_WIDTH 480
#define CARD_HEIGHT 520
#define CARD_CORNER_RADIUS 16
#define AVATAR_SIZE 80
#define INPUT_HEIGHT 48
#define BUTTON_HEIGHT 44
#define BUTTON_CORNER_RADIUS 8

/* ============================================================================
   State Management
   ========================================================================== */
typedef enum {
    LOGIN_MODE_BOOT = 0,
    LOGIN_MODE_CHANGE_PASSWORD = 1
} login_mode_t;

static login_mode_t current_mode = LOGIN_MODE_BOOT;
static char password_input[PASS_MAX_LEN] = {0};
static char new_password[PASS_MAX_LEN] = {0};
static char confirm_password[PASS_MAX_LEN] = {0};
static int password_len = 0;
static bool password_error = false;
static char error_message[128] = {0};
static uint64_t last_cursor_tick = 0;
static bool cursor_visible = true;
static int screen_result = 0;  /* 0=running, 1=success, 2=cancel */
static uint64_t animation_tick = 0;
static int input_focus = 0;  /* 0=password, 1=new, 2=confirm */

/* ============================================================================
   Drawing Utilities
   ========================================================================== */
static void draw_rounded_rect(int x, int y, int w, int h, int r, uint32_t color) {
    /* Use the real rounded-rect primitive (same one boot_animation.c uses)
       instead of the old flat-corner approximation, so the login card
       actually looks rounded and matches the boot animation's rendering. */
    vga_fill_rounded_rect(x, y, w, h, r, color);
}

static void draw_text_centered(int x, int y, const char* text, uint32_t color) {
    vga_draw_string(x, y, text, color, COLOR_BG_DARK);
}

static void draw_avatar(int x, int y, int size) {
    /* Draw a simple user avatar (circle with initials), using the same
       navy/cyan palette and circle primitive as the boot animation, plus
       a thin outer glow ring so it reads as part of the same "C-OS" theme. */
    uint32_t avatar_color = 0x00466EA5; /* matches boot animation ring color */
    uint32_t glow_color = 0x00589CEE;
    int cx = x + size / 2;
    int cy = y + size / 2;
    int r = size / 2;

    vga_draw_circle(cx, cy, r + 2, glow_color);
    vga_fill_circle(cx, cy, r, avatar_color);

    /* Draw user icon (simple "U" shape) */
    uint32_t icon_color = 0xFFFFFF;
    int icon_x = x + size/2 - 4;
    int icon_y = y + size/2 - 6;
    
    vga_fill_rect(icon_x, icon_y, 2, 8, icon_color);
    vga_fill_rect(icon_x + 6, icon_y, 2, 8, icon_color);
    vga_fill_rect(icon_x + 2, icon_y + 6, 4, 2, icon_color);
}

/* ============================================================================
   Input Field Drawing
   ========================================================================== */
static void draw_input_field(int x, int y, int w, int h, const char* placeholder, 
                             const char* value, bool focused, bool error) {
    /* Background */
    uint32_t bg_color = focused ? COLOR_INPUT_FOCUS : COLOR_INPUT_BG;
    uint32_t border_color = error ? COLOR_ERROR : (focused ? COLOR_INPUT_FOCUS : COLOR_INPUT_BORDER);
    
    draw_rounded_rect(x, y, w, h, 4, bg_color);
    
    /* Border */
    vga_draw_rounded_rect(x, y, w, h, 4, border_color);
    
    /* Text */
    uint32_t text_color = value && value[0] ? COLOR_TEXT_PRIMARY : COLOR_TEXT_SECONDARY;
    const char* display_text = (value && value[0]) ? value : placeholder;
    
    vga_draw_string(x + 12, y + (h - 8) / 2, display_text, text_color, bg_color);
    
    /* Cursor (if focused) */
    if (focused && cursor_visible && value) {
        int cursor_x = x + 12 + strlen(value) * 8;
        vga_fill_rect(cursor_x, y + 8, 2, h - 16, COLOR_TEXT_PRIMARY);
    }
}

/* ============================================================================
   Button Drawing
   ========================================================================== */
static void draw_button(int x, int y, int w, int h, const char* text, 
                        bool hovered, bool active) {
    uint32_t bg_color = active ? COLOR_BUTTON_ACTIVE : (hovered ? COLOR_BUTTON_HOVER : COLOR_BUTTON_BG);
    
    draw_rounded_rect(x, y, w, h, BUTTON_CORNER_RADIUS, bg_color);
    
    /* Text */
    int text_x = x + (w - strlen(text) * 8) / 2;
    int text_y = y + (h - 8) / 2;
    vga_draw_string(text_x, text_y, text, COLOR_TEXT_PRIMARY, bg_color);
}

/* ============================================================================
   Main Login Screen Render
   ========================================================================== */
static void render_login_screen(void) {
    /* Clear screen */
    vga_fill_rect(0, 0, PW_SCREEN_W, PW_SCREEN_H, COLOR_BG_DARK);
    
    /* Calculate card position (centered) */
    int card_x = (PW_SCREEN_W - CARD_WIDTH) / 2;
    int card_y = (PW_SCREEN_H - CARD_HEIGHT) / 2;
    
    /* Draw card background */
    draw_rounded_rect(card_x, card_y, CARD_WIDTH, CARD_HEIGHT, CARD_CORNER_RADIUS, COLOR_CARD_BG);
    
    /* Draw card border */
    vga_draw_rounded_rect(card_x, card_y, CARD_WIDTH, CARD_HEIGHT, CARD_CORNER_RADIUS, COLOR_CARD_BORDER);
    
    /* Draw avatar */
    int avatar_x = card_x + (CARD_WIDTH - AVATAR_SIZE) / 2;
    int avatar_y = card_y + 30;
    draw_avatar(avatar_x, avatar_y, AVATAR_SIZE);
    
    /* Draw title */
    draw_text_centered(card_x + CARD_WIDTH/2 - 60, avatar_y + AVATAR_SIZE + 20, "C-OS Login", COLOR_TEXT_PRIMARY);
    
    /* Draw username label */
    draw_text_centered(card_x + 20, avatar_y + AVATAR_SIZE + 60, "Username", COLOR_TEXT_SECONDARY);
    
    /* Draw username field (read-only) */
    int input_x = card_x + 20;
    int input_y = avatar_y + AVATAR_SIZE + 80;
    draw_input_field(input_x, input_y, CARD_WIDTH - 40, INPUT_HEIGHT, "root", "root", false, false);
    
    /* Draw password label */
    draw_text_centered(card_x + 20, input_y + INPUT_HEIGHT + 20, "Password", COLOR_TEXT_SECONDARY);
    
    /* Draw password field */
    int pwd_input_y = input_y + INPUT_HEIGHT + 40;
    draw_input_field(input_x, pwd_input_y, CARD_WIDTH - 40, INPUT_HEIGHT, 
                     "Enter password", password_input, input_focus == 0, password_error);
    
    /* Draw error message if present */
    if (password_error && error_message[0]) {
        vga_draw_string(input_x, pwd_input_y + INPUT_HEIGHT + 10, error_message, COLOR_ERROR, COLOR_BG_DARK);
    }
    
    /* Draw login button */
    int button_x = card_x + 20;
    int button_y = pwd_input_y + INPUT_HEIGHT + 50;
    draw_button(button_x, button_y, CARD_WIDTH - 40, BUTTON_HEIGHT, "Login", false, false);
    
    /* Draw footer text */
    draw_text_centered(card_x + 20, button_y + BUTTON_HEIGHT + 30, "C-OS 4.0.8 alpha", COLOR_TEXT_SECONDARY);
}

/* ============================================================================
   Password Change Screen Render
   ========================================================================== */
static void render_password_change_screen(void) {
    /* Clear screen */
    vga_fill_rect(0, 0, PW_SCREEN_W, PW_SCREEN_H, COLOR_BG_DARK);
    
    /* Calculate card position */
    int card_x = (PW_SCREEN_W - CARD_WIDTH) / 2;
    int card_y = (PW_SCREEN_H - CARD_HEIGHT) / 2;
    
    /* Draw card */
    draw_rounded_rect(card_x, card_y, CARD_WIDTH, CARD_HEIGHT, CARD_CORNER_RADIUS, COLOR_CARD_BG);
    vga_draw_rounded_rect(card_x, card_y, CARD_WIDTH, CARD_HEIGHT, CARD_CORNER_RADIUS, COLOR_CARD_BORDER);
    
    /* Draw title */
    draw_text_centered(card_x + 20, card_y + 30, "Change Password", COLOR_TEXT_PRIMARY);
    
    /* Current password field */
    int input_x = card_x + 20;
    int input_y = card_y + 80;
    draw_text_centered(input_x, input_y - 30, "Current Password", COLOR_TEXT_SECONDARY);
    draw_input_field(input_x, input_y, CARD_WIDTH - 40, INPUT_HEIGHT, 
                     "Enter current password", password_input, input_focus == 0, false);
    
    /* New password field */
    int new_pwd_y = input_y + INPUT_HEIGHT + 40;
    draw_text_centered(input_x, new_pwd_y - 30, "New Password", COLOR_TEXT_SECONDARY);
    draw_input_field(input_x, new_pwd_y, CARD_WIDTH - 40, INPUT_HEIGHT, 
                     "Enter new password", new_password, input_focus == 1, false);
    
    /* Confirm password field */
    int confirm_pwd_y = new_pwd_y + INPUT_HEIGHT + 40;
    draw_text_centered(input_x, confirm_pwd_y - 30, "Confirm Password", COLOR_TEXT_SECONDARY);
    draw_input_field(input_x, confirm_pwd_y, CARD_WIDTH - 40, INPUT_HEIGHT, 
                     "Confirm new password", confirm_password, input_focus == 2, false);
    
    /* Error message */
    if (password_error && error_message[0]) {
        vga_draw_string(input_x, confirm_pwd_y + INPUT_HEIGHT + 10, error_message, COLOR_ERROR, COLOR_BG_DARK);
    }
    
    /* Buttons */
    int button_y = confirm_pwd_y + INPUT_HEIGHT + 50;
    draw_button(input_x, button_y, (CARD_WIDTH - 40 - 10) / 2, BUTTON_HEIGHT, "Save", false, false);
    draw_button(input_x + (CARD_WIDTH - 40 - 10) / 2 + 10, button_y, (CARD_WIDTH - 40 - 10) / 2, BUTTON_HEIGHT, "Cancel", false, false);
}

/* ============================================================================
   Input Handling
   ========================================================================== */
static void handle_keyboard_input(keyboard_event_t ev) {
    if (ev.scancode == 0x1C) {  /* Enter */
        screen_result = 1;
    } else if (ev.scancode == 0x0E) {  /* Backspace */
        if (password_len > 0) {
            password_len--;
            password_input[password_len] = '\0';
            password_error = false;
        }
    } else if (ev.ascii >= 32 && ev.ascii <= 126 && password_len < PASS_MAX_LEN - 1) {
        password_input[password_len++] = ev.ascii;
        password_input[password_len] = '\0';
        password_error = false;
    }
}

/* ============================================================================
   Public API
   ========================================================================== */
void password_screen_enhanced_init(void) {
    serial_puts("[PASSWORD] Initializing enhanced password screen\n");
    memset(password_input, 0, sizeof(password_input));
    memset(new_password, 0, sizeof(new_password));
    memset(confirm_password, 0, sizeof(confirm_password));
    password_len = 0;
    password_error = false;
    screen_result = 0;
    current_mode = LOGIN_MODE_BOOT;
    animation_tick = 0;
}

void password_screen_enhanced_render(void) {
    animation_tick++;
    
    /* Update cursor visibility */
    uint64_t current_tick = get_timer_ticks();
    if (current_tick - last_cursor_tick > 25) {  /* ~500ms blink */
        cursor_visible = !cursor_visible;
        last_cursor_tick = current_tick;
    }
    
    /* Render appropriate screen */
    if (current_mode == LOGIN_MODE_BOOT) {
        render_login_screen();
    } else {
        render_password_change_screen();
    }
}

void password_screen_enhanced_handle_input(keyboard_event_t ev) {
    handle_keyboard_input(ev);
}

int password_screen_enhanced_get_result(void) {
    return screen_result;
}

void password_screen_enhanced_set_error(const char* message) {
    password_error = true;
    if (message) {
        strncpy(error_message, message, sizeof(error_message) - 1);
        error_message[sizeof(error_message) - 1] = '\0';
    }
}

void password_screen_enhanced_clear_error(void) {
    password_error = false;
    error_message[0] = '\0';
}

const char* password_screen_enhanced_get_password(void) {
    return password_input;
}

bool password_screen_enhanced_show(void) {
    password_screen_enhanced_init();
    serial_puts("[PASSWORD] Entering blocking login loop\n");

    // Check if a password is set. If not, prompt to set one.
    if (!storage_has_password()) {
        serial_puts("[PASSWORD] No password set. Prompting user to set one.\n");
        // For now, we'll set a default password for demo purposes if none exists.
        // In a real scenario, this would transition to a password creation screen.
        if (!storage_set_password("1234")) {
            serial_puts("[PASSWORD] Failed to set initial password.\n");
            return false;
        }
        serial_puts("[PASSWORD] Default password '1234' set.\n");
    }

    uint64_t start_tick = get_timer_ticks();

    while (screen_result == 0) {
        /* Blink cursor */
        uint64_t current_tick = get_timer_ticks();
        if (current_tick - last_cursor_tick > 25) {
            cursor_visible = !cursor_visible;
            last_cursor_tick = current_tick;
        }

        if (current_tick - start_tick >= BOOT_PASSWORD_TIMEOUT_TICKS) {
            serial_puts("[PASSWORD] Boot screen timeout; continuing without login\n");
            return false;
        }

        password_screen_enhanced_render();
        vga_flip();

        keyboard_poll();
        if (keyboard_has_event()) {
            keyboard_event_t ev = keyboard_get_event();
            if (ev.pressed) {
                password_screen_enhanced_handle_input(ev);
            }
        }

        if (screen_result == 1) {
            if (storage_verify_password(password_input)) {
                serial_puts("[PASSWORD] Login successful\n");
                permission_set_uid(0); // Assuming root UID is 0
                return true;
            } else {
                serial_puts("[PASSWORD] Login failed\n");
                password_error = true;
                strcpy(error_message, "Invalid password");
                memset(password_input, 0, sizeof(password_input));
                password_len = 0;
                screen_result = 0;
            }
        }
    }
    return false;
}

void password_screen_enhanced_reset(void) {
    memset(password_input, 0, sizeof(password_input));
    password_len = 0;
    password_error = false;
    screen_result = 0;
    cursor_visible = true;
}
