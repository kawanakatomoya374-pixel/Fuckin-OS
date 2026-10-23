/* cos_pcache.h - a shared cache of read-only user pages.
 *
 * Every process that runs the same program or loads the same .c-osll gets its own copy of the code and the read-only data: the loader reads the file
 * and copies it into fresh frames. For a library used by ten programs that is ten copies of identical bytes. After a segment has been given its final
 * (non-writable) permissions, cos_pcache_share() looks for an earlier page with exactly the same contents (hash, then a full compare - a collision can never
 * hand a program somebody else's bytes) and maps that frame instead, freeing the private copy. Because the test is on the CONTENT, it needs no knowledge of
 * relocations: a page that a relocation changed simply differs between processes and is not shared.
 *
 * A shared frame counts the page-table entries that map it. cos_pcache_release() drops one count and frees the frame with the last. Pages that are
 * writable (or that user code can ever make writable) are never offered. */
#ifndef COS_PCACHE_H
#define COS_PCACHE_H
#include <stdint.h>
#include <stdbool.h>

/* The page at user address `virt` (mapped in the active directory, final permissions `flags`, not writable) is made shared. Returns true when the
 * page is now backed by a shared frame (found or newly registered), false when it was left private (cache full). */
bool cos_pcache_share(uint64_t virt, uint64_t flags);

/* The frame `phys` is no longer mapped by the page being torn down. Returns true when it was a shared frame (its count dropped; it is freed at 0), false when it
 * is an ordinary private frame the caller must free itself. */
bool cos_pcache_release(uint64_t phys);

/* Statistics: frames in the cache, page-table entries mapping them, and the frames saved by sharing (entries - frames). */
void cos_pcache_stats(uint32_t *frames, uint32_t *mappings);
void cos_pcache_reset(void);   /* forget all entries without freeing frames (host simulator only) */
#endif
