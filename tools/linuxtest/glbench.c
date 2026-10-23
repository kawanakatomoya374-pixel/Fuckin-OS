/* glbench.c - times mcgl.c (the CPU renderer of mc-classic-c) on synthetic geometry: small and large textured quads, so the cost of a triangle and the
 * cost of a pixel can be told apart. Built with the host compiler against the renderer's source; run on the host and in C-OS (docs/MC_CLASSIC.md). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glew.h>
#include "mcgl.c"
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static void quad(float x, float y, float z, float s) {
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex3f(x, y, z); glTexCoord2f(0.0625f, 0); glVertex3f(x + s, y, z);
    glTexCoord2f(0.0625f, 0.0625f); glVertex3f(x + s, y + s, z); glTexCoord2f(0, 0.0625f); glVertex3f(x, y + s, z);
    glEnd();
}
int main(int argc, char **argv) {
    double spin = argc > 1 ? atof(argv[1]) : 0;                  /* seconds to keep drawing small quads (for sampling a profile) */
    mcgl_resize(854, 480);
    static uint32_t tex[256 * 256]; for (int i = 0; i < 256 * 256; ++i) tex[i] = 0xFF000000u | (uint32_t)(i * 2654435761u) >> 8;
    GLuint id; glGenTextures(1, &id); glBindTexture(GL_TEXTURE_2D, id); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
    glEnable(GL_TEXTURE_2D); glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.0f);
    glViewport(0, 0, 854, 480);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); gluPerspective(70, 854.0 / 480.0, 0.05, 1000);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glColor3f(0.8f, 0.8f, 0.8f);
    const int NS = 20000, NB = 400;
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    double t0 = now();
    for (int i = 0; i < NS; ++i) { float x = -3.0f + (i % 200) * 0.03f, y = -1.5f + ((i / 200) % 100) * 0.03f; quad(x, y, -3.0f - (i % 7) * 0.01f, 0.025f); }
    if (spin > 0) { double ts = now(); while (now() - ts < spin) for (int i = 0; i < NS; ++i) { float x = -3.0f + (i % 200) * 0.03f, y = -1.5f + ((i / 200) % 100) * 0.03f; quad(x, y, -3.0f - (i % 7) * 0.01f, 0.025f); } }
    double t1 = now();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    double t2 = now();
    for (int i = 0; i < NB; ++i) quad(-1.2f + (i % 4) * 0.01f, -0.7f, -2.0f - (i % 5) * 0.001f, 2.4f);          /* ~ 500x280 px each, overdrawn */
    double t3 = now();
    printf("small quads (~3px): %d in %.0f ms: %.2f us per quad (2 triangles)\n", NS, (t1 - t0) * 1000, (t1 - t0) * 1e6 / NS);
    printf("big quads (~%d px): %d in %.0f ms: %.1f ms per quad -> %.0f ns per pixel\n", 500 * 280, NB, (t3 - t2) * 1000, (t3 - t2) * 1000 / NB, (t3 - t2) * 1e9 / NB / (500.0 * 280));
    return 0;
}
