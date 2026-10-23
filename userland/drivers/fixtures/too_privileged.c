/* A BASIC module that imports a DEVICE function (port I/O): the loader must refuse it - its level is lower than the export's. */
#include "cosys.h"
static int tp_init(void) { return cosys_outb(0x80, 1); }
static void tp_exit(void) { }
COSYS_MODULE("too_privileged", "1.0", COSYS_LEVEL_BASIC, tp_init, tp_exit);
