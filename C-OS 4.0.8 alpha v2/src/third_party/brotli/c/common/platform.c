/* Copyright 2016 Google Inc. All Rights Reserved.

   Distributed under MIT license.
   See file LICENSE for detail or copy at https://opensource.org/licenses/MIT
*/

#if defined(COS_KERNEL)
#include "memory.h"
#else
#include <stdlib.h>
#endif

#include <brotli/types.h>

#include "platform.h"

/* Default brotli_alloc_func. C-OS normally supplies explicit request-local
 * callbacks, but keeping the default kernel-safe leaves every public decoder
 * API linkable in the freestanding image. */
void* BrotliDefaultAllocFunc(void* opaque, size_t size) {
  BROTLI_UNUSED(opaque);
#if defined(COS_KERNEL)
  return kmalloc(size);
#else
  return malloc(size);
#endif
}

/* Default brotli_free_func */
void BrotliDefaultFreeFunc(void* opaque, void* address) {
  BROTLI_UNUSED(opaque);
#if defined(COS_KERNEL)
  if (address != NULL) kfree(address);
#else
  free(address);
#endif
}
