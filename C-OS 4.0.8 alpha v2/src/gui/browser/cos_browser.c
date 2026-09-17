/**
 * cos_browser.c - C-OS browser entry point
 *
 * This file intentionally stays thin: it exposes a stable C-OS facing API and
 * forwards the work to the existing browser engine. That keeps the browser easy
 * to embed while preserving the current renderer / parser / HTTP stack.
 */

#include "cos_browser.h"

int cos_browser_init(void) {
    return modern_browser_init();
}

int cos_browser_open(const char* url) {
    return modern_browser_open(url);
}

void cos_browser_close(void) {
    modern_browser_close();
}

void cos_browser_refresh(void) {
    modern_browser_refresh();
}

cos_browser_state_t cos_browser_get_state(void) {
    return modern_browser_get_state();
}

const char* cos_browser_get_url(void) {
    return modern_browser_get_url();
}

const char* cos_browser_get_title(void) {
    return modern_browser_get_title();
}

int cos_browser_navigate(const char* url) {
    return browser_navigate(url);
}

int cos_browser_back(void) {
    return browser_back();
}

int cos_browser_forward(void) {
    return browser_forward();
}

int cos_browser_reload(void) {
    return browser_reload();
}

int cos_browser_home(void) {
    return browser_home();
}

void cos_browser_scroll(int delta) {
    browser_scroll(delta);
}

void cos_browser_handle_keypress(char key, int scancode) {
    browser_handle_keypress(key, scancode);
}

void cos_browser_handle_click(int mx, int my, int button) {
    browser_handle_click(mx, my, button);
}

void cos_browser_draw(int x, int y, int w, int h) {
    browser_draw(x, y, w, h);
}
