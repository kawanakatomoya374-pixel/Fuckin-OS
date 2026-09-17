/**
 * cos_browser.h - C-OS drop-in browser API
 *
 * This is a small namespaced wrapper around the existing modern browser core.
 * It gives the OS a stable entry point for launching, rendering, and handling input
 * without having to know about the internal browser implementation.
 */

#ifndef COS_BROWSER_H
#define COS_BROWSER_H

#include <stdint.h>
#include <stdbool.h>
#include "modern_browser.h"

typedef browser_state_t cos_browser_state_t;

int cos_browser_init(void);
int cos_browser_open(const char* url);
void cos_browser_close(void);
void cos_browser_refresh(void);

cos_browser_state_t cos_browser_get_state(void);
const char* cos_browser_get_url(void);
const char* cos_browser_get_title(void);

/* Navigation */
int cos_browser_navigate(const char* url);
int cos_browser_back(void);
int cos_browser_forward(void);
int cos_browser_reload(void);
int cos_browser_home(void);

/* Input and drawing */
void cos_browser_scroll(int delta);
void cos_browser_handle_keypress(char key, int scancode);
void cos_browser_handle_click(int mx, int my, int button);
void cos_browser_draw(int x, int y, int w, int h);

#endif /* COS_BROWSER_H */
