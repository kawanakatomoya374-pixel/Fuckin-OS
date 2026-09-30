/* Host-test stub of the kernel's mm/paging.h.
 *
 * Only the names cos_elf.c actually uses. The point of the harness is to
 * compile the REAL loader source unmodified against a simulated MMU, so
 * that what is tested is the shipping code and not a transcription of
 * it. */
#ifndef STUB_PAGING_H
#define STUB_PAGING_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PAGE_SIZE     4096
#define PAGE_PRESENT  0x001
#define PAGE_RW       0x002
#define PAGE_USER     0x004

typedef uint64_t page_directory_t;

bool     paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t paging_virt_to_phys(uint64_t v);
uint64_t paging_alloc_physical(void);
void     paging_free_physical(uint64_t p);
int      paging_protect_page(uint64_t v, uint64_t f);

void              paging_switch_directory(page_directory_t *dir);
page_directory_t *paging_get_current_directory(void);
void              paging_unmap_page(uint64_t v);

/* Harness-only introspection, used by the tests to assert on the final
 * page permissions the loader applied. */
uint64_t sim_page_flags(uint64_t v);
void     sim_reset(void);
uint64_t sim_mapped_pages(void);

#endif
