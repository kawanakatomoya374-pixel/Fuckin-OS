#ifndef _COS_NETDB_H
#define _COS_NETDB_H
#include "_cos_libc_cfg.h"
_COS_BEGIN
#include "netinet/in.h"
struct addrinfo { int ai_flags; int ai_family; int ai_socktype; int ai_protocol; socklen_t ai_addrlen; struct sockaddr *ai_addr; char *ai_canonname; struct addrinfo *ai_next; };
struct hostent { char *h_name; char **h_aliases; int h_addrtype; int h_length; char **h_addr_list; };
#define h_addr h_addr_list[0]
#define AI_PASSIVE 1
#define AI_CANONNAME 2
#define AI_NUMERICHOST 4
#define EAI_NONAME -2
#define EAI_FAIL -4
#define EAI_MEMORY -10
#define EAI_SERVICE -8
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int code);
struct hostent *gethostbyname(const char *name);
_COS_END
#endif
