#ifndef _COS_ARPA_INET_H
#define _COS_ARPA_INET_H
#include "_cos_libc_cfg.h"
_COS_BEGIN
#include "../netinet/in.h"
int inet_pton(int af, const char *src, void *dst);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
in_addr_t inet_addr(const char *s);
char *inet_ntoa(struct in_addr a);
int inet_aton(const char *s, struct in_addr *a);
_COS_END
#endif
