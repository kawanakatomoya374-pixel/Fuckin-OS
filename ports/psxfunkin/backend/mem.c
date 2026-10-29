/* PSXFunkin on C-OS: memory. The game's Mem_Alloc / Mem_Free are malloc and free, with one difference: the PlayStation build frees the chart of a stage that
 * is a plain array inside the program (the heap of a PC build would crash). Blocks carry a magic header and Mem_Free ignores anything without it. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define PSXF_MAGIC 0x5053584D454D4631ull
void *psxf_alloc(size_t size)
{
	uint64_t *p = (uint64_t*)malloc(size + 16); if (!p) return NULL;
	p[0] = PSXF_MAGIC; p[1] = size; return p + 2;
}
void psxf_free(void *ptr)
{
	if (!ptr) return;
	uint64_t *p = (uint64_t*)ptr - 2;
	if (p[0] != PSXF_MAGIC) return;                    /* not ours: static data */
	p[0] = 0; free(p);
}
