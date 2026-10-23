#ifndef COS_SCREENSHOT_H
#define COS_SCREENSHOT_H
#include <stdbool.h>
void screenshot_begin(void);
void screenshot_handle_mouse(void);
void screenshot_draw_overlay(void);
void screenshot_service_before_overlay(void);
#endif
