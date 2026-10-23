/* mcgl.h - the framebuffer side of mcgl.c (the OpenGL 1.x subset renderer). */
#ifndef MCGL_H
#define MCGL_H
#include <stdint.h>
int  mcgl_resize(int w, int h);           /* size of the internal picture; 0 on success */
int  mcgl_width(void);
int  mcgl_height(void);
void mcgl_present(uint32_t *dst, int stride_pixels, int w, int h);   /* copy (and scale) into the window's pixels */
#endif
