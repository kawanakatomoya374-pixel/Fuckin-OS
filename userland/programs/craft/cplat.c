/* cplat.c - assets, settings and the multiplayer stubs of the C-OS port of Craft. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cos.h"
#include "r3d.h"
#include "cplat.h"
#include "client.h"

/* PNG decoder from userland/lib/stbi_all.c */
unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *channels_in_file, int desired_channels);
void stbi_image_free(void *p);

extern const unsigned char craft_texture_png[]; extern const unsigned int craft_texture_png_len;
extern const unsigned char craft_font_png[];    extern const unsigned int craft_font_png_len;
extern const unsigned char craft_sky_png[];     extern const unsigned int craft_sky_png_len;
extern const unsigned char craft_sign_png[];    extern const unsigned int craft_sign_png_len;

int craft_load_assets(void) {
    int w, h, n;
    unsigned char *p;
    p = stbi_load_from_memory(craft_texture_png, (int)craft_texture_png_len, &w, &h, &n, 4); if (!p) return 1; r3d_set_atlas(p, w, h); stbi_image_free(p);
    p = stbi_load_from_memory(craft_font_png, (int)craft_font_png_len, &w, &h, &n, 4);       if (!p) return 2; r3d_set_font(p, w, h);  stbi_image_free(p);
    p = stbi_load_from_memory(craft_sign_png, (int)craft_sign_png_len, &w, &h, &n, 4);       if (!p) return 3; r3d_set_sign(p, w, h);  stbi_image_free(p);
    p = stbi_load_from_memory(craft_sky_png, (int)craft_sky_png_len, &w, &h, &n, 4);         if (!p) return 4; r3d_set_sky(p, w, h);   stbi_image_free(p);
    return 0;
}

/* ---- vertex buffers: Craft's GLuint handles index a table of malloc'd float arrays ---- */
/* (the table itself lives in util.c: gen_buffer / del_buffer) */

int get_access_token(char *result, int length, char *username, char *identity_token) { (void)result; (void)length; (void)username; (void)identity_token; return 0; }

/* libcos has double acos() but no float variant; cube.c's sphere builder wants one. */
double acos(double x);
float acosf(float x) { return (float)acos((double)x); }
double asin(double x);
float asinf(float x) { return (float)asin((double)x); }
