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
void gui_boot_animation_init(void);        /* Initialize before boot sequence */
void gui_boot_animation_run(void);         /* Run the boot animation */
bool gui_boot_animation_completed(void);   /* Check if animation finished */
bool gui_boot_animation_completed(void);   /* Legacy alias */
bool gui_boot_is_animating(void);          /* Check if still animating */
int  gui_boot_get_result(void);            /* Get animation result */
void gui_boot_force_complete(void);        /* Force completion for error recovery */
void gui_boot_animation_reset(void);       /* Reset for reboot */

/* Phase tracking */
gui_boot_phase_t gui_boot_get_current_phase(void);
const char* gui_boot_get_phase_name(void);

/* Progress callback type */
typedef void (*gui_boot_progress_callback_t)(int percentage, const char* phase);
void gui_boot_set_progress_callback(gui_boot_progress_callback_t cb);

#endif