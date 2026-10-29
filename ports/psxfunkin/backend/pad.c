/* PSXFunkin on C-OS: the pad is the keyboard (plat_keys() already speaks in PAD_* bits, see cos/plat_cos.c for the key map). */
#include "pad.h"
#include "../plat.h"
Pad pad_state, pad_state_2;
void Pad_Init(void) { memset(&pad_state, 0, sizeof pad_state); memset(&pad_state_2, 0, sizeof pad_state_2); pad_state.left_x = pad_state.left_y = pad_state.right_x = pad_state.right_y = 0x80; }
void Pad_Quit(void) { }
void Pad_Update(void)
{
	u16 held = (u16)plat_keys();
	pad_state.press = (u16)(held & ~pad_state.held); pad_state.held = held;
	pad_state_2.held = pad_state_2.press = 0;
}
