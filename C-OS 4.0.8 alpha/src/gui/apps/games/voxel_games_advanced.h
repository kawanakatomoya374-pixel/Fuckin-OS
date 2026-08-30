#ifndef VOXEL_GAMES_ADVANCED_H
#define VOXEL_GAMES_ADVANCED_H

#include "gui.h"

#ifdef __cplusplus
extern "C" {
#endif

void voxel_games_handle_key(int idx, const keyboard_event_t* ev);
void voxel_games_handle_click(int idx, int mx, int my);
void voxel_games_draw(int idx);
void voxel_games_save_window_state(const window_t* w);

#ifdef __cplusplus
}
#endif

#endif /* VOXEL_GAMES_ADVANCED_H */
