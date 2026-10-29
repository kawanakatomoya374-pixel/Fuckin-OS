/* PSXFunkin on C-OS: the "console". */
#include "psx.h"
#include "../plat.h"
int my_argc; char **my_argv;
void plat_set_data_dir(const char *d);
void plat_check_data(void);
void PSX_Init(void) { if (my_argc > 1) plat_set_data_dir(my_argv[1]); plat_check_data(); }
void PSX_Quit(void) { }
boolean PSX_Running(void) { return plat_running() ? true : false; }
#include <stdarg.h>
void FntPrint(const char *format, ...) { (void)format; }
void MsgPrint(const char *format, ...)             /* the game's fatal errors end here: show them (the PlayStation shows them on the screen) */
{
	char b[512]; va_list ap; va_start(ap, format); vsnprintf(b, sizeof b, format, ap); va_end(ap);
	plat_log("[psxf] "); plat_log(b); plat_log("\n");
}
