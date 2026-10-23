/* mcstbi.h - pre-included into every source of the port: stb_image is built without stdio, stbi_load() is in mcrt.c. */
#ifndef MCSTBI_H
#define MCSTBI_H
unsigned char *stbi_load(const char *filename, int *x, int *y, int *channels_in_file, int desired_channels);
#endif
