/*
 * cos_js_os_api.h - GUI-facing portion of the privileged C-OS JavaScript API.
 *
 * This header deliberately exposes no QuickJS runtime/context type. The GUI
 * only composites validated drawing commands after its normal desktop scene.
 */
#ifndef COS_JS_OS_API_H
#define COS_JS_OS_API_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Composite the validated privileged-JS drawing overlay into the active GUI
 * backbuffer. It must be called only by the GUI owner, after normal scene
 * rendering and before the matching vga_flip(). Returns true when at least
 * one command was composited. */
bool cos_js_os_draw_overlay(void);

#ifdef __cplusplus
}
#endif

#endif /* COS_JS_OS_API_H */
