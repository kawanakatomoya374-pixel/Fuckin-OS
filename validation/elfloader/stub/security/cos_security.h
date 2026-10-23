/* host-test stand-in for src/kernel/security/cos_security.h: no SMAP on the host, and a deterministic "random" value so runs repeat */
#ifndef COS_SECURITY_H
#define COS_SECURITY_H
#include <stdint.h>
uint64_t cos_random_u64(void);
extern volatile uint8_t cos_smap_active;
static inline void cos_user_access_begin(void) {}
static inline void cos_user_access_end(void) {}
#endif
