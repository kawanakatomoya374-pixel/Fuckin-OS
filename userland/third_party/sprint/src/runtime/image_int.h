/* .spc image codec internals shared by image.c, sha256.c and verify.c. */
#ifndef IMAGE_INT_H
#define IMAGE_INT_H

/* Program image (.spc): SHA-256, encoder, decoder and verifier (SPEC 16, 17.9). */
#include "sp.h"

/* functions shared between the parts of this stage */
void mod_sha(const ModInfo *m, uint8_t out[32]);
void *xalloc(size_t n);

#endif
