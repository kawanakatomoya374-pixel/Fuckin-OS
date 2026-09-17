/*
 * C-OS TinyCC Configuration
 * Minimal config for embedded environment - Kernel mode
 */
#ifndef TCC_CONFIG_H
#define TCC_CONFIG_H

#define TCC_VERSION "0.9.27"
#define CONFIG_TCC_STATIC 1
#define CONFIG_TCC_PREPROCESSOR 1
#define CONFIG_TCC_ASM 1
#define CONFIG_TCC_LIBTCC 1

/* Disable features not needed in C-OS kernel environment */
#undef CONFIG_TCC_BACKTRACE
#undef CONFIG_TCC_GPROF
#undef CONFIG_TCC_BCHECK
#undef CONFIG_TCC_DEBUG
#undef CONFIG_TCC_RUNTIME
#undef CONFIG_TCC_ELF
#undef CONFIG_TCC_COBJ

/* No DWARF support in kernel */
#undef CONFIG_TCC_DWARF

/* Paths */
#define TCC_INCLUDE_PATH "/storage/include"
#define TCC_LIB_PATH "/storage/lib"

#endif /* TCC_CONFIG_H */
