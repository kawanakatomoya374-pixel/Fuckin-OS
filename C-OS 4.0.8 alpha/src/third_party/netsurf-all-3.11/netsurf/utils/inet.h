/*
 * Copyright 2016 Vincent Sanders <vince@netsurf-browser.org>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * internet structures and defines
 *
 * This allows the obtaining of standard bsd sockets and associated
 * functions in a uniform way despite any oddities in headers and
 * supported API between OS.
 *
 * \note This functionality was previously provided as a side effect of the
 *  utils config header include.
 */

#ifndef _NETSURF_UTILS_INET_H_
#define _NETSURF_UTILS_INET_H_

#include "utils/config.h"

#if defined(COS_KERNEL)

/* C-OS has its own networking stack (kernel/api/net_api.c) rather
 * than BSD sockets, so there's no <sys/socket.h>/<netinet/in.h>/
 * <arpa/inet.h> to reach for here (and, being a freestanding kernel
 * build, no real one to accidentally pull in from the host either -
 * without this branch, the #else path below would silently succeed
 * at finding the *host* Linux sandbox's real system headers, which
 * assume a hosted glibc environment this kernel doesn't have and
 * fail deep inside them in confusing ways). Nothing in this build
 * calls inet_aton()/inet_pton(), so unlike the branches below, this
 * one declares neither - if something ever needs dotted-quad address
 * parsing, that's a small, self-contained function to hand-write
 * against C-OS's own net_api rather than something needing this
 * header at all.
 */

/* Nothing in this build actually performs select()-style fd
 * multiplexing yet (that's specifically how the curl-based fetcher in
 * content/fetchers/curl.c integrates with an event loop, and that
 * fetcher isn't part of this build - see PORTING_NOTES.md), but a
 * couple of header-only signatures still mention fd_set* as a
 * parameter/struct-member type (content/fetch.h's fetch_fdset(),
 * content/fetchers.h's `fdset` table entry) even though this build
 * never defines or calls anything that would dereference one. Rather
 * than guard out those signatures (and risk quietly changing a
 * struct's shape for whenever it does matter later), this is a
 * minimal stand-in so the type name itself resolves. */
/* libwapcaplet may include the hosted sys/select.h before this
 * compatibility header, even for the freestanding target.  Reuse its
 * declaration when present; otherwise retain the opaque C-OS stand-in. */
#ifndef _SYS_SELECT_H
typedef struct { unsigned char cos_unused_fd_set_placeholder; } fd_set;
#endif

#elif defined(HAVE_POSIX_INET_HEADERS)

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#else

#include <winsock2.h>
#include <ws2tcpip.h>

#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT WSAEAFNOSUPPORT
#endif

#endif


#if !defined(COS_KERNEL)
#ifndef HAVE_INETATON
int inet_aton(const char *cp, struct in_addr *inp);
#endif

#ifndef HAVE_INETPTON
int inet_pton(int af, const char *src, void *dst);
#endif
#endif

#endif
