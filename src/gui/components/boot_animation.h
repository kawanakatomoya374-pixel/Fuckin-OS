#ifndef GUI_BOOT_ANIMATION_H
#define GUI_BOOT_ANIMATION_H

#include <stdbool.h>
#include <stdint.h>

/* Boot phases */
typedef enum {
    GUI_BOOT_PHASE_INIT = 0,
    GUI_BOOT_PHASE_LOADING,
    GUI_BOOT_PHASE_TRANSITION,
    GUI_BOOT_PHASE_DESKTOP,
    GUI_BOOT_PHASE_COMPLETE
} gui_boot_phase_t;

/* Core animation functions */
void gui_boot_progress(const char *label); /* One real frame, drawn synchronously between kernel_main's own init steps - see the definition for why this exists instead of a fixed-duration animation */
bool gui_boot_animation_completed(void);   /* Check if animation finished */
bool gui_boot_animation_completed(void);   /* Legacy alias */

/* Phase tracking */

/* Progress callback type */
typedef void (*gui_boot_progress_callback_t)(int percentage, const char* phase);

#endif