/*
Copyright (c) 2014 David Hubbard

This program is free software: you can redistribute it and/or modify it under the terms of
the GNU Affero General Public License version 3, as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU Affero General Public License version 3 for more details.

You should have received a copy of the GNU Affero General Public License version 3 along with
this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include "iface.h"

#ifndef _SIZEOF_ADDR_IFREQ
#define _SIZEOF_ADDR_IFREQ(x) sizeof(x)
#endif

#define USE_GETIFADDRS

#ifdef USE_GETIFADDRS
#include <sys/types.h>
#include <ifaddrs.h>

static int foreach_if_af_inet(struct ifaddrs * ifa, foreach_if_cb cb, void * ctx)
{
	for (; ifa; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;

		u32 ip_addr = ((struct sockaddr_in *) ifa->ifa_addr)->sin_addr.s_addr;
		if (!ip_addr) {
			fprintf(stderr, " [%s no IP]", ifa->ifa_name);
			continue;
		}

		u32 netmask = ((struct sockaddr_in *) ifa->ifa_netmask)->sin_addr.s_addr;
		if (cb(ifa->ifa_name, ip_addr, netmask, ctx)) return 1;
	}
	return 0;
}

int foreach_if(int sock_to_kernel, foreach_if_cb cb, void * ctx)
{
	(void) sock_to_kernel;

	struct ifaddrs * p;
	if (getifaddrs(&p)) {
		fprintf(stderr, "foreach_if: getifaddrs() failed: %d %s\n", errno, strerror(errno));
		return 1;
	}

	int r = foreach_if_af_inet(p, cb, ctx);
	freeifaddrs(p);
	return r;
}

#else /* USE_GETIFADDRS */

int foreach_if(int sock_to_kernel, foreach_if_cb cb, void * ctx)
{
	struct ifconf ifc;
	size_t max = 4096;
	ifc.ifc_buf = (typeof(ifc.ifc_buf)) malloc(max);
	if (!ifc.ifc_buf) {
		fprintf(stderr, "foreach_if: malloc(%zu) failed\n", max);
		return 1;
	}
	ifc.ifc_len = max;	// init ifc_len so for() does not stop before calling ioctl(SIOCGIFCONF) at least once
	for (; (size_t) ifc.ifc_len >= max; max += 4096) {
		ifc.ifc_len = max;	// give the buffer size to ioctl(SIOCGIFCONF)
		ifc.ifc_buf = (typeof(ifc.ifc_buf)) realloc(ifc.ifc_buf, max);
		if (!ifc.ifc_buf) {
			fprintf(stderr, "foreach_if: realloc(%zu) failed\n", max);
			return 1;
		}
		memset(ifc.ifc_buf, 0, max);
		if (ioctl(sock_to_kernel, SIOCGIFCONF, &ifc)) {
			fprintf(stderr, "foreach_if: SIOCGIFCONF failed: %d %s\n", errno, strerror(errno));
			free(ifc.ifc_buf);
			return 1;
		}
	}

	char * p;
	struct ifreq * ifr;
#ifdef USE_IDX
	max = 0;
	size_t idx = 0;
	for (p = ifc.ifc_buf; p < ifc.ifc_buf + ifc.ifc_len; ifr = (typeof(ifr)) p, p += _SIZEOF_ADDR_IFREQ(*ifr))
		max++;
#endif

	for (p = ifc.ifc_buf; p < ifc.ifc_buf + ifc.ifc_len; p += _SIZEOF_ADDR_IFREQ(*ifr)
#ifdef USE_IDX
		, idx++
#endif
		)
	{
		ifr = (typeof(ifr)) p;
		if (ioctl(sock_to_kernel, SIOCGIFFLAGS, p)) {
			fprintf(stderr, "foreach_if: SIOCGIFFLAGS(%s) failed: %d %s\n", ifr->ifr_name, errno, strerror(errno));
			free(ifc.ifc_buf);
			return 1;
		}

		if (!(ifr->ifr_flags & IFF_UP) || !(ifr->ifr_flags & IFF_RUNNING)) {
			fprintf(stderr, " [%s down]", ifr->ifr_name);
			continue;
		}

		if (ioctl(sock_to_kernel, SIOCGIFADDR, p)) {
			fprintf(stderr, "foreach_if: SIOCGIFADDR(%s) failed: %d %s\n", ifr->ifr_name, errno, strerror(errno));
			free(ifc.ifc_buf);
			return 1;
		}

		u32 ip_addr = ((struct sockaddr_in *) &(ifr->ifr_addr))->sin_addr.s_addr;
		if (!ip_addr) {
			fprintf(stderr, " [%s no IP]", ifr->ifr_name);
			continue;
		}

		if (ioctl(sock_to_kernel, SIOCGIFNETMASK, p)) {
			fprintf(stderr, "foreach_if: SIOCGIFNETMASK(%s) failed: %d %s\n", ifr->ifr_name, errno, strerror(errno));
			free(ifc.ifc_buf);
			return 1;
		}
		u32 netmask = ((struct sockaddr_in *) &(ifr->ifr_addr))->sin_addr.s_addr;

		if (cb(ifr->ifr_name, ip_addr, netmask, ctx)) {
			free(ifc.ifc_buf);
			return 1;
		}
	}

	free(ifc.ifc_buf);
	return 0;
}
#endif /* USE_GETIFADDRS */

void __ip_printf(char * s, size_t len, u32 ip, const char * funcname, unsigned long line)
{
	if (inet_ntop(AF_INET, &ip, s, len)) return;

	fprintf(stderr, "%s:%lu: ip_printf failed: %d %s\n", funcname, line, errno, strerror(errno));
	exit(1);
}

int get_hw_addr(int sock_to_kernel, const char * ifname, u8 hwaddr[6])
{
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if (ioctl(sock_to_kernel, SIOCGIFHWADDR, &ifr)) {
		fprintf(stderr, "SIOCGIFHWADDR failed: %d %s\n", errno, strerror(errno));
		return 1;
	}

	for (unsigned i = 0; i < sizeof(hwaddr)/sizeof(hwaddr[0]); i++)
		hwaddr[i] = ifr.ifr_hwaddr.sa_data[i];
	return 0;
}
