/* cplat.h - the C-OS side of the Craft port: assets, settings, and the multiplayer calls (not available here). */
#ifndef CPLAT_H
#define CPLAT_H
int craft_load_assets(void);       /* decodes the embedded PNG textures into r3d; 0 on success */
const float *buffer_data(unsigned int buffer);
#endif
