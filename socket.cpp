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

#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <new>
#include "mpgts.h"

using namespace tuner_ns;

static int udp_bind(int sock_to_if, char * addrstr, u32 ip_addr)
{
	int v = 1;
	if (setsockopt(sock_to_if, SOL_SOCKET, SO_BROADCAST, (char *)&v, sizeof(v))) {
		fprintf(stderr, "UDP socket %s: SO_BROADCAST failed: %d %s\n", addrstr, errno, strerror(errno));
		return 1;
	}

	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = ip_addr;
	//sin.sin_port = 0;	// done by memset()
	if (bind(sock_to_if, (struct sockaddr *) &sin, sizeof(sin))) {
		fprintf(stderr, "UDP socket %s: bind() failed: %d %s\n", addrstr, errno, strerror(errno));
		return 1;
	}
	return 0;
}

static u32 tuner_calc_crc(u8 * pkt, size_t len)
{
	u32 crc = (u32) -1;
	for (size_t i = 0; i < len; i++, pkt++) {
		u8 x = ((u8) crc) ^ *pkt;
		crc >>= 8;
		if (x & 0x01) crc ^= 0x77073096;
		if (x & 0x02) crc ^= 0xee0e612c;
		if (x & 0x04) crc ^= 0x076dc419;
		if (x & 0x08) crc ^= 0x0edb8832;
		if (x & 0x10) crc ^= 0x1db71064;
		if (x & 0x20) crc ^= 0x3b6e20c8;
		if (x & 0x40) crc ^= 0x76dc4190;
		if (x & 0x80) crc ^= 0xedb88320;
	}
	return ~crc;
}

static void pkt_add_crc(u8 * pkt, size_t len, u8 pkt_type)
{
	pkt[0] = 0;		// big endian
	pkt[1] = pkt_type;
	len -= 8;		// header and CRC are not counted in len
	pkt[2] = len >> 8;	// big endian
	pkt[3] = len & 0xff;
	u32 crc = tuner_calc_crc(pkt, len + 4);
	pkt[len + 4] = (u8) (crc >> 0);	// little endian
	pkt[len + 5] = (u8) (crc >> 8);
	pkt[len + 6] = (u8) (crc >> 16);
	pkt[len + 7] = (u8) (crc >> 24);
}

static int udp_pkt_sendto(int sock_to_if, char * src_addrstr, u32 dest_ip, unsigned port,
	u8 * pkt, size_t len)
{
	char dstr[256]; ip_printf(dstr, dest_ip);

	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = dest_ip;
	sin.sin_port = htons(port);

	int r = sendto(sock_to_if, pkt, len, 0, (struct sockaddr *) &sin, sizeof(sin));
	if (r < 0) {
		fprintf(stderr, "udp_pkt_sendto(%s, %zu) from %s failed: %d %s\n", dstr, len, src_addrstr, errno, strerror(errno));
		return 1;
	} else if ((size_t) r != len) {
		fprintf(stderr, "udp_pkt_sendto(%s) from %s sent %d not %zu\n", dstr, src_addrstr, r, len);
		return 1;
	}
	return 0;
}

static int udp_pkt_recv(int sock_to_if, char * src_addrstr, u8 * rx, size_t * rxlen, u32 * rxaddr, u32 time_msec)
{
	struct timespec t_out;
	t_out.tv_nsec = time_msec*1000000L;
	t_out.tv_sec = 0;
	if (t_out.tv_nsec >= 1000000000L) {
		t_out.tv_nsec -= 1000000000L;
		t_out.tv_sec++;
	}

	fd_set rfds, wfds, efds;
	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_ZERO(&efds);
	FD_SET(sock_to_if, &rfds);
	ssize_t r = pselect(sock_to_if + 1, &rfds, &wfds, &efds, &t_out, 0 /*sigmask*/);
	if (r < 0) {
		fprintf(stderr, "pselect(%s) failed: %d %s\n", src_addrstr, errno, strerror(errno));
		return 1;
	}
	if (!r || !FD_ISSET(sock_to_if, &rfds)) {
		*rxlen = 0;
		return 0;
	}

	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	socklen_t sinsize = sizeof(sin);
	r = recvfrom(sock_to_if, rx, *rxlen, 0 /*flags*/, (struct sockaddr *) &sin, &sinsize);
	if (r < 0) {
		fprintf(stderr, "udp_pkt_recv(%s) failed: %d %s\n", src_addrstr, errno, strerror(errno));
		return 1;
	}
	if (r == 0) {
		fprintf(stderr, "udp_pkt_recv(%s): connection closed\n", src_addrstr);
		return 1;
	}

	// check CRC
	u32 crc = tuner_calc_crc(rx, r - 4);
	if (rx[r - 4] != (u8) (crc >> 0) ||
		rx[r - 3] != (u8) (crc >> 8) ||
		rx[r - 2] != (u8) (crc >> 16) ||
		rx[r - 1] != (u8) (crc >> 24))
	{
		fprintf(stderr, "udp_pkt_recv(%s): got %u bytes, want crc %08x got %02x%02x%02x%02x\n", src_addrstr,
			(unsigned) r, crc, rx[r-1], rx[r-2], rx[r-3], rx[r-4]);
		return 1;
	}

	// check len
	if (((((u32) rx[2]) << 8) | rx[3]) + 8 != r) {
		fprintf(stderr, "udp_pkt_recv(%s): got %u bytes, len says %u bytes\n", src_addrstr, (unsigned) r,
			((((u32) rx[2]) << 8) | rx[3]) + 8);
		return 1;
	}

	// remove the 2-byte len
	memmove(&rx[2], &rx[4], r - 6);

	*rxlen = r - 6;
	*rxaddr = sin.sin_addr.s_addr;
	return 0;
}

struct tunerfind2_ctx {
	int sock_to_kernel;
	unsigned iface_found, debug;
	unsigned list_use, list_max;
	mpgts * list;
};
#define hdhomerun_port (65001)

static int tunerfind2(const char * if_name, u32 ip_addr, u32 netmask, void * ctx)
{
	tunerfind2_ctx * list = (typeof(list)) ctx;

	// only send on interfaces with link-local IPv4 address (169.254.0.0/16)
	if ((ntohl(ip_addr) & 0xffff0000) != 0xa9fe0000 || ntohl(netmask) != 0xffff0000) {
		if (list->debug) fprintf(stderr, " %s:not 169.254", if_name);
		return 0;
	}

	list->iface_found++;

	char addrstr[256]; ip_printf(addrstr, ip_addr);

	if (list->debug) fprintf(stderr, " %s:%s", if_name, addrstr);

	{
		u8 hwaddr[6];
		if (get_hw_addr(list->sock_to_kernel, if_name, hwaddr)) return 1;
		if (hwaddr[0] != 0 || hwaddr[1] != 0x21 || hwaddr[2] != 0x33) {
			fprintf(stderr, "Warn: %s mac address %02x:%02x:%02x:%02x:%02x:%02x\n", if_name,
				hwaddr[0], hwaddr[1], hwaddr[2], hwaddr[3], hwaddr[4], hwaddr[5]);
			fprintf(stderr, "Warn: tuners will only send video if you \""
				"sudo ifconfig %s hw ether 00:21:33:%02x:%02x:%02x\"\n", if_name,
				hwaddr[3], hwaddr[4], hwaddr[5]);
		}
	}

	int sock_to_if = ::socket(AF_INET, SOCK_DGRAM, 0 /*protocol: not used*/);
	if (sock_to_if == -1) {
		fprintf(stderr, "UDP socket %s: socket() failed: %d %s\n", addrstr, errno, strerror(errno));
		return 1;
	}
	if (udp_bind(sock_to_if, addrstr, ip_addr)) {
		close(sock_to_if);
		return 1;
	}

	// send UDP packet to broadcast address
	u8 pkt[] = {
			0, 0, 0, 0,	// header
			2, 4,	// tag: device ID   len: device ID
			0xff, 0xff, 0xff, 0xff,	// device ID, big endian
			1, 4,	// tag: device type  len: device type
			0, 0, 0, 2,	// device type, big endian
			0, 0, 0, 0,	// CRC
		};
	// yes, this is constant data, so the CRC could theoretically be hard coded, but just do it the normal way
	pkt_add_crc(pkt, sizeof(pkt), 2 /*discover request*/);

	if (udp_pkt_sendto(sock_to_if, addrstr, ip_addr | ~netmask /* broadcast address */, hdhomerun_port,
		pkt, sizeof(pkt)))
	{
		close(sock_to_if);
		return 1;
	}

	u32 rxaddr = 0;
	u8 rx[4096];
	for (;;) {
		size_t rxlen = sizeof(rx);
		if (udp_pkt_recv(sock_to_if, addrstr, rx, &rxlen, &rxaddr, 50 /*ms*/)) {
			close(sock_to_if);
			return 1;
		}
		if (!rxlen) return 0;

		// remember all tuners that respond to the broadcast packet

		char dstr[256]; ip_printf(dstr, rxaddr);
		if (rxlen != 22) {
			fprintf(stderr, " >> %s got %zu bytes\n", dstr, rxlen);
			return 1;
		}

		if (list->list_use >= list->list_max) {
			list->list = (typeof(list->list)) realloc(list->list, sizeof(*list->list) * (list->list_max += 8));
			if (!list->list) {
				fprintf(stderr, "realloc list->list failed\n");
				return 1;
			}
		}
		new(&list->list[list->list_use]) mpgts(rxaddr, &rx[16], ip_addr);
		list->list_use++;
	}
	return 0;
}

mpgts * socket::find(unsigned * num_tuners, unsigned debug /*= 0*/)
{
	tunerfind2_ctx list;
	memset(&list, 0, sizeof(list));
	list.debug = debug;
	list.list = (typeof(list.list)) malloc(sizeof(*list.list) * (list.list_max = 8));
	if (!list.list) {
		fprintf(stderr, "malloc list.list failed\n");
		return 0;
	}

	list.sock_to_kernel = ::socket(AF_INET, SOCK_DGRAM, 0 /*protocol: not used*/);
	if (list.sock_to_kernel == -1) {
		fprintf(stderr, "create UDP socket: %d %s\n", errno, strerror(errno));
		return 0;
	}

	int r = foreach_if(list.sock_to_kernel, tunerfind2, &list);
	if (!r) {
		if (!list.iface_found) {
			fprintf(stderr, "Error: no interfaces have 169.254.0.0/16\n");
			r = 1;
		} else {
			if (debug) fprintf(stderr, "\n");
		}
	}

	::close(list.sock_to_kernel);

	if (r) {
		free(list.list);
		list.list = 0;
		list.list_use = 0;
	}

	*num_tuners = list.list_use;
	return list.list;
}


int socket::open()
{
	ip_printf(ipstr, get_ip());

	sock = ::socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		fprintf(stderr, "socket::open(%s): socket failed: %d %s\n", ipstr, errno, strerror(errno));
		return 1;
	}

	sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = get_ip();
	sin.sin_port = htons(hdhomerun_port);
	if (connect(sock, (struct sockaddr *) &sin, sizeof(sin))) {
		fprintf(stderr, "socket::open(%s): connect failed: %d %s\n", ipstr, errno, strerror(errno));
		::close(sock);
		sock = -1;
		return 1;
	}
	return 0;
}

int socket::write(u8 * pkt, size_t pktlen, u8 pkt_type)
{
	pkt_add_crc(pkt, pktlen, pkt_type);
	while (pktlen) {
		ssize_t r = ::write(sock, pkt, pktlen);
		if (r < 0) {
			fprintf(stderr, "socket::write(%s) failed: %d %s\n", ipstr, errno, strerror(errno));
			return 1;
		}
		pkt += r;
		pktlen -= r;
	}
	return 0;
}

int socket::read(u8 * pkt, size_t * pktlen)
{
	size_t n = *pktlen;
	int nread;
	if (ioctl(sock, FIONREAD, &nread) < 0) {
		fprintf(stderr, "socket::read(%s) FIONREAD failed: %d %s\n", ipstr, errno, strerror(errno));
		return 1;
	}
	if ((size_t) nread < n) n = nread;
	if (n < 4) {	// not enough bytes received to get a packet
		*pktlen = 0;
		return 0;
	}
	n = 4;

	u8 * buf = pkt;
	while (n) {
		ssize_t r = ::read(sock, buf, n);
		if (r < 0) {
			fprintf(stderr, "socket::read(%s) failed: %d %s\n", ipstr, errno, strerror(errno));
			return 1;
		}
		buf += r;
		n -= r;
	}
	n = (((u32) pkt[2]) << 8) | pkt[3];
	n += 8-4;	// already read 4 bytes
	*pktlen = n;	// and 4 bytes will get stripped off after CRC is checked, so *pktlen == n
	buf = pkt + 4;
	while (n) {
		ssize_t r = ::read(sock, buf, n);
		if (r < 0) {
			fprintf(stderr, "socket::read(%s) failed: %d %s\n", ipstr, errno, strerror(errno));
			return 1;
		}
		buf += r;
		n -= r;
	}
	n = *pktlen;

	u32 crc = tuner_calc_crc(pkt, n);
	if (pkt[n] != (u8) (crc >> 0) ||
		pkt[n + 1] != (u8) (crc >> 8) ||
		pkt[n + 2] != (u8) (crc >> 16) ||
		pkt[n + 3] != (u8) (crc >> 24))
	{
		fprintf(stderr, "socket::read(%s) got %u bytes, want crc %08x got %02x%02x%02x%02x\n", ipstr,
			(unsigned) (n + 4), crc, pkt[n+3], pkt[n+2], pkt[n+1], pkt[n]);
		return 1;
	}
	return 0;
}

void socket::close()
{
	if (sock == -1) return;
	::close(sock);
	sock = -1;
}

u8 * socket::write_then_read(u8 * pkt, size_t pktlen, size_t * rxlen)
{
	if (write(pkt, pktlen, 0x0c /*tuner request*/)) return 0;

	struct timeval tv_start;
	gettimeofday(&tv_start, 0);
	ssize_t n;
	fd_set rfds;
	do {
		struct timeval tv_now;
		gettimeofday(&tv_now, 0);

		unsigned long time_left = (tv_now.tv_sec - tv_start.tv_sec)*1000000 + tv_now.tv_usec - tv_start.tv_usec;
		if (time_left > 400*1000) {
			fprintf(stderr, "socket::write_then_read: pselect timed out\n");
			return 0;
		}
		time_left = 400*1000 - time_left;

		struct timespec t_out;
		t_out.tv_nsec = time_left*1000LU;
		t_out.tv_sec = 0;
		if (t_out.tv_nsec >= 1000000000L) {
			t_out.tv_nsec -= 1000000000L;
			t_out.tv_sec++;
		}

		fd_set wfds, efds;
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_ZERO(&efds);
		FD_SET(sock, &rfds);
		n = pselect(sock + 1, &rfds, &wfds, &efds, &t_out, 0 /*sigmask*/);
		if (n < 0) {
			fprintf(stderr, "socket::write_then_read: pselect failed: %d %s\n", errno, strerror(errno));
			return 0;
		}
	} while (!n || !FD_ISSET(sock, &rfds));

	n = *rxlen + 4;
	u8 * rx = (u8 *) malloc(n);
	if (!rx) {
		fprintf(stderr, "socket::write_then_read: malloc(%zu) failed\n", n);
		return 0;
	}
	if (read(rx, (size_t *) &n)) {
		free(rx);
		return 0;
	}
	if (!n) {
		fprintf(stderr, "socket::write_then_read: pselect said read, read returned 0\n");
		free(rx);
		*rxlen = 0;
		return 0;
	}
	if (n < 4) {
		// should not happen: socket::read() always returns at least 4
		fprintf(stderr, "socket::write_then_read: read says %zu\n", n);
		free(rx);
		return 0;
	}
	if (rx[0] != 0 || rx[1] != 0x0d /*tuner response*/) {
		fprintf(stderr, "socket::write_then_read: sent 000c got %02x%02x back\n", rx[0], rx[1]);
		free(rx);
		return 0;
	}
	*rxlen = n;
	return rx;
}

int socket::get_gpio(u32 * val)
{
	u8 pkt[] = {
			0,0,0,0,	// header
			0x0f, 0xf3,	// CPU bus (0x0ff2), read (| 1)
			2, 4,		// get GPIO
			0,0,0,0,	// CRC
		};
	size_t n = 2;
	u8 * rx = write_then_read(pkt, sizeof(pkt), &n);
	if (!rx) return 1;
	if (n != 2 + 4) {
		fprintf(stderr, "get_gpio() returned %zu\n", n);
		return 1;
	}
	*val = ((u32) rx[4] << 8) | rx[5];
	free(rx);
	return 0;
}

int socket::set_gpio(u32 val)
{
	if (val & ~0xffff) {
		fprintf(stderr, "set_gpio(%04x) invalid\n", val);
		return 1;
	}

	u8 pkt[] = {
			0,0,0,0,	// header
			0x0f, 0xf2,	// CPU bus (0x0ff2), write (| 0)
			4,		// set GPIO
			(u8) (val >> 8), (u8) val,
			0,0,0,0,	// CRC
		};
	size_t n = 8;
	u8 * rx = write_then_read(pkt, sizeof(pkt), &n);
	if (!rx) return 1;
	if (n != 4) {
		fprintf(stderr, "set_gpio(%04x) fault: %zu\n", val, n);
		return 1;
	}
	free(rx);
	return 0;
}

int socket::get_demod8(u8 ch, u32 addr, u8 * val)
{
	if ((addr & ~0xffff) || ch > 2) {
		fprintf(stderr, "get_demod8(%u, %04x) invalid\n", ch, addr);
		return 1;
	}

	// talking to an LG DT3305
	u8 pkt[] = {
			0,0,0,0,	// header
			ch, 0xb3,	// Demod bus (ch*256 + 0xb2), read (| 1)
			1,		// read 1 byte
			(u8) (addr >> 8), (u8) addr,
			0,0,0,0,	// CRC
		};
	size_t n = 1;
	u8 * rx = write_then_read(pkt, sizeof(pkt), &n);
	if (!rx) return 1;
	if (n != 1 + 4) {
		fprintf(stderr, "get_demod8(%u, %04x) fault: %zu\n", ch, addr, n);
		return 1;
	}
	*val = rx[4];
	free(rx);
	return 0;
}

int socket::set_demod8(u8 ch, u32 addr, u8 val)
{
	if ((addr & ~0xffff) || ch > 2) {
		fprintf(stderr, "set_demod8(%u, %04x) invalid\n", ch, addr);
		return 1;
	}

	// talking to an LG DT3305
	u8 pkt[] = {
			0,0,0,0,	// header
			ch, 0xb2,	// Demod bus (ch*256 + 0xb2), write (| 0)
			(u8) (addr >> 8), (u8) addr, val,
			0,0,0,0,	// CRC
		};
	size_t n = 8;
	u8 * rx = write_then_read(pkt, sizeof(pkt), &n);
	if (!rx) return 1;
	if (n != 4) {
		fprintf(stderr, "set_demod8(%u, %04x) fault: %zu\n", ch, addr, n);
		return 1;
	}
	free(rx);
	return 0;
}

int socket::get_demod16(u8 ch, u32 addr, u32 * val)
{
	if ((addr & ~0xffff) || ch > 2) {
		fprintf(stderr, "get_demod16(%u, %04x) invalid\n", ch, addr);
		return 1;
	}

	// talking to an LG DT3305
	u8 pkt[] = {
			0,0,0,0,	// header
			ch, 0xb3,	// Demod bus (ch*256 + 0xb2), read (| 1)
			2,		// read 2 bytes
			(u8) (addr >> 8), (u8) addr,
			0,0,0,0,	// CRC
		};
	size_t n = 2;
	u8 * rx = write_then_read(pkt, sizeof(pkt), &n);
	if (!rx) return 1;
	if (n != 2 + 4) {
		fprintf(stderr, "get_demod16(%u, %04x) fault: %zu\n", ch, addr, n);
		return 1;
	}
	*val = ((u32) rx[4] << 8) | rx[5];
	free(rx);
	return 0;
}

int socket::set_demod16(u8 ch, u32 addr, u32 val)
{
	if ((addr & ~0xffff) || ch > 2 || (val & ~0xffff)) {
		fprintf(stderr, "set_demod16(%u, %04x, %04x) invalid\n", ch, addr, val);
		return 1;
	}

	// talking to an LG DT3305
	u8 pkt[] = {
			0,0,0,0,	// header
			ch, 0xb2,	// Demod bus (ch*256 + 0xb2), write (| 0)
			(u8) (addr >> 8), (u8) addr,
			(u8) (val >> 8), (u8) val,
			0,0,0,0,	// CRC
		};
	size_t n = 8;
	u8 * rx = write_then_read(pkt, sizeof(pkt), &n);
	if (!rx) return 1;
	if (n != 4) {
		fprintf(stderr, "set_demod16(%u, %04x) fault: %zu\n", ch, addr, n);
		return 1;
	}
	free(rx);
	return 0;
}

int socket::get_demod24(u8 ch, u32 addr, u32 * val)
{
	if ((addr & ~0xffff) || ch > 2) {
		fprintf(stderr, "get_demod24(%u, %04x) invalid\n", ch, addr);
		return 1;
	}

	// talking to an LG DT3305
	u8 pkt[] = {
			0,0,0,0,	// header
			ch, 0xb3,	// Demod bus (ch*256 + 0xb2), read (| 1)
			3,		// read 3 bytes
			(u8) (addr >> 8), (u8) addr,
			0,0,0,0,	// CRC
		};
	size_t n = 3;
	u8 * rx = write_then_read(pkt, sizeof(pkt), &n);
	if (!rx) return 1;
	if (n != 3 + 4) {
		fprintf(stderr, "get_demod24(%u, %04x) fault: %zu\n", ch, addr, n);
		return 1;
	}
	*val = ((u32) rx[4] << 16) | ((u32) rx[5] << 8) | rx[6];
	free(rx);
	return 0;
}

int socket::set_demod24(u8 ch, u32 addr, u32 val)
{
	if ((addr & ~0xffff) || ch > 2 || (val & ~0xffffff)) {
		fprintf(stderr, "set_demod24(%u, %04x, %04x) invalid\n", ch, addr, val);
		return 1;
	}

	// talking to an LG DT3305
	u8 pkt[] = {
			0,0,0,0,	// header
			ch, 0xb2,	// Demod bus (ch*256 + 0xb2), write (| 0)
			(u8) (addr >> 8), (u8) addr,
			(u8) (val >> 16), (u8) (val >> 8), (u8) val,
			0,0,0,0,	// CRC
		};
	size_t n = 8;
	u8 * rx = write_then_read(pkt, sizeof(pkt), &n);
	if (!rx) return 1;
	if (n != 4) {
		fprintf(stderr, "set_demod24(%u, %04x) fault: %zu\n", ch, addr, n);
		return 1;
	}
	free(rx);
	return 0;
}

int socket::get_demod32(u8 ch, u32 addr, u32 * val)
{
	if ((addr & ~0xffff) || ch > 2) {
		fprintf(stderr, "get_demod32(%u, %04x) invalid\n", ch, addr);
		return 1;
	}

	// talking to an LG DT3305
	u8 pkt[] = {
			0,0,0,0,	// header
			ch, 0xb3,	// Demod bus (ch*256 + 0xb2), read (| 1)
			4,		// read 4 bytes
			(u8) (addr >> 8), (u8) addr,
			0,0,0,0,	// CRC
		};
	size_t n = 4;
	u8 * rx = write_then_read(pkt, sizeof(pkt), &n);
	if (!rx) return 1;
	if (n != 4 + 4) {
		fprintf(stderr, "get_demod32(%u, %04x) fault: %zu\n", ch, addr, n);
		return 1;
	}
	*val = ((u32) rx[4] << 24) | ((u32) rx[5] << 16) | ((u32) rx[6] << 8) | rx[7];
	free(rx);
	return 0;
}

int socket::set_demod32(u8 ch, u32 addr, u32 val)
{
	if ((addr & ~0xffff) || ch > 2) {
		fprintf(stderr, "set_demod32(%u, %04x, %04x) invalid\n", ch, addr, val);
		return 1;
	}

	// talking to an LG DT3305
	u8 pkt[] = {
			0,0,0,0,	// header
			ch, 0xb2,	// Demod bus (ch*256 + 0xb2), write (| 0)
			(u8) (addr >> 8), (u8) addr,
			(u8) (val >> 24), (u8) (val >> 16), (u8) (val >> 8), (u8) val,
			0,0,0,0,	// CRC
		};
	size_t n = 8;
	u8 * rx = write_then_read(pkt, sizeof(pkt), &n);
	if (!rx) return 1;
	if (n != 4) {
		fprintf(stderr, "set_demod32(%u, %04x) fault: %zu\n", ch, addr, n);
		return 1;
	}
	free(rx);
	return 0;
}

int socket::get_demodN(u8 ch, u32 addr, u8 * arr, u8 len)
{
	if ((addr & ~0xffff) || ch > 2 || !len) {
		fprintf(stderr, "get_demodN(%u, %04x, %u) invalid\n", ch, addr, len);
		return 1;
	}

	// talking to an LG DT3305
	u8 pkt[] = {
			0,0,0,0,	// header
			ch, 0xb3,	// Demod bus (ch*256 + 0xb2), read (| 1)
			len,
			(u8) (addr >> 8), (u8) addr,
			0,0,0,0,	// CRC
		};
	size_t n = 4;
	u8 * rx = write_then_read(pkt, sizeof(pkt), &n);
	if (!rx) return 1;
	if (n != (size_t) (4 + len)) {
		fprintf(stderr, "get_demodN(%u, %04x, %u) fault: %zu\n", ch, addr, len, n);
		return 1;
	}
	memcpy(arr, &rx[4], len);
	free(rx);
	return 0;
}

int socket::set_demodN(u8 ch, u32 addr, u8 * arr, u8 len)
{
	if ((addr & ~0xffff) || ch > 2 || !len) {
		fprintf(stderr, "set_demodN(%u, %04x, %u) invalid\n", ch, addr, len);
		return 1;
	}

	// talking to an LG DT3305
	u8 pkt[len + 12];
	memset(pkt, 0, sizeof(pkt));
	// pkt[0..3] header
	pkt[4] = ch;
	pkt[5] = 0xb2;	// Demod bus (ch*256 + 0xb2), write (| 0)
	pkt[6] = (u8) (addr >> 8);
	pkt[7] = (u8) addr;
	memcpy(&pkt[8], arr, len);
	size_t n = 8;
	u8 * rx = write_then_read(pkt, len + 12, &n);
	if (!rx) return 1;
	if (n != 4) {
		fprintf(stderr, "set_demodN(%u, %04x, %u) fault: %zu\n", ch, addr, len, n);
		return 1;
	}
	free(rx);
	return 0;
}

int socket::reset_demod(u8 ch, unsigned reset_ms)
{
	if (ch > 2) {
		fprintf(stderr, "reset_demod(%u) invalid\n", ch);
		return 1;
	}
	u8 b;
	if (get_demod8(ch, 2, &b)) return 1;
	b &= ~1;
	if (set_demod8(ch, 2, b)) return 1;
	usleep(reset_ms*1000);
	b |= 1;
	if (set_demod8(ch, 2, b)) return 1;
	return 0;
}
