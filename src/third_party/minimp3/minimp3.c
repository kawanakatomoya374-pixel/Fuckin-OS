/*
 * minimp3.c - single translation-unit implementation for MiniMP3
 *
 * This keeps the decoder usable from regular build systems that expect a
 * C compilation unit rather than relying on header-only implementation
 * macros scattered across the kernel tree.
 */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "minimp3.h"
