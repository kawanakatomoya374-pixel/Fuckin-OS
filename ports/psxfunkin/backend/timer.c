/* PSXFunkin on C-OS: time. The game counts seconds in 10-bit fixed point (about a millisecond); the real elapsed time of every frame is used, from the
 * microsecond clock, so the game stays in step with the audio even when a frame is late. */
#include "timer.h"
#include "../plat.h"
u32 frame_count, animf_count;
fixed_t timer_sec, timer_dt;
static uint64_t t_last, virt_us;
void Timer_Init(void) { frame_count = animf_count = 0; timer_sec = timer_dt = 0; t_last = plat_time_us(); virt_us = 0; }
void Timer_Tick(void)
{
	uint64_t now = plat_time_us(); frame_count++;
	uint64_t dt = now - t_last; if (dt > 100000) dt = 100000;                      /* a stall (window dragged, file loading) does not make the game jump */
	t_last = now; virt_us += dt;
	fixed_t next = (fixed_t)((virt_us * FIXED_UNIT) / 1000000u);                    /* from the total, so the rounding of one frame does not add up */
	timer_dt = next - timer_sec; timer_sec = next;
	animf_count = (u32)(((u64)timer_sec * 24) >> FIXED_SHIFT);
}
void Timer_Reset(void) { Timer_Tick(); timer_dt = 0; }
