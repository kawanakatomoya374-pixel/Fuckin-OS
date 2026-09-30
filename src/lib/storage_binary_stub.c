/**
 * storage_binary_stub.c - Binary symbol stubs for missing implementations
 */

// Binary symbol stubs for embedded storage image
extern char _binary_build_storage_img_start[];
extern char _binary_build_storage_img_end[];

// Dummy symbols to satisfy linker
char _binary_build_storage_img_start[1] = {0};
char _binary_build_storage_img_end[1] = {0};
