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

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "mpgts.h"

using namespace tuner_ns;

void * mpgts::thread_main()
{
	for (;;) {
		fd_set rfds, wfds, efds;
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_ZERO(&efds);
		FD_SET(udp_sock[0], &rfds);
		FD_SET(udp_sock[1], &rfds);

		struct timespec t_out;
		t_out.tv_sec = 0;
		t_out.tv_nsec = 100*1000000L;
		int max_sock = udp_sock[0];
		if (udp_sock[1] > udp_sock[0]) max_sock = udp_sock[1];
		ssize_t r = pselect(max_sock + 1, &rfds, &wfds, &efds, &t_out, 0 /*sigmask*/);
		if (r < 0) {
			fprintf(stderr, "mpegts: pselect() failed: %d %s\n", errno, strerror(errno));
			return 0;
		}

		unsigned i;
		for (i = 0; i < 2; i++) if (want_reset[i]) {
			want_reset[i] = 0;
			if (atsc[i].init()) return 0;
		}

		if (!r) continue;

		for (i = 0; i < 2; i++) if (FD_ISSET(udp_sock[i], &rfds)) {
			u8 rx[4096];

			struct sockaddr_in sin;
			memset(&sin, 0, sizeof(sin));
			socklen_t sinlen = sizeof(sin);
			r = recvfrom(udp_sock[i], rx, sizeof(rx), 0 /*flags*/, (struct sockaddr *) &sin, &sinlen);
			if (r < 0) {
				fprintf(stderr, "mpegts: recvfrom%u failed: %d %s\n", i, errno, strerror(errno));
				return 0;
			}
			if (r == 0) {
				fprintf(stderr, "mpegts: recvfrom%u got 0 bytes\n", i);
				continue;
			}

			if (r != 1328) {
				fprintf(stderr, "Warn: mpegts%u: %u byte packet (not 1328)\n", i, (unsigned) r);
				continue;
			}

			//u32 rx_seq = ((u32) rx[0] << 24) + ((u32) rx[1] << 16) + ((u32) rx[2] << 8) + rx[3];
			// 32 bits of 0 at rx[4]
			// 32 bits of 0 at rx[8]
			// 7 x 188-byte TS packets start at rx[12]
			u32 ofs = 12;
			u8 * p = rx + ofs;
			for (; ofs < r; ofs += 188 /*size of a TS packet*/, p += 188)
				if (atsc[i].thread_demux(p)) return 0;
		}
	}

	return 0;
}

void * mpgts::thread_wrapper(void * p) { return static_cast<mpgts *>(p)->thread_main(); }
int mpgts::open()
{
	if (tun.open()) return 1;
	if (tun.init()) return 1;
	if (atsc[0].init()) return 1;
	if (atsc[1].init()) return 1;

	unsigned i;
	for (i = 0; i < 2; i++) {
		udp_sock[i] = ::socket(AF_INET, SOCK_DGRAM, 0 /*protocol: not used*/);
		if (udp_sock[i] == -1) {
			fprintf(stderr, "mpgts::open(%u): UDP socket failed: %d %s\n", i, errno, strerror(errno));
			return 1;
		}

		struct sockaddr_in sin;
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_addr.s_addr = tun.get_myip();
		//sin.sin_port = 0;	// done by memset()
		if (bind(udp_sock[i], (struct sockaddr *) &sin, sizeof(sin))) {
			fprintf(stderr, "mpgts::open(%u): bind() failed: %d %s\n", i, errno, strerror(errno));
			return 1;
		}

		// get the port number the system assigned to udp_sock[i]
		socklen_t sinlen = sizeof(sin);
		if (getsockname(udp_sock[i], (struct sockaddr *) &sin, &sinlen) || sinlen > sizeof(sin)) {
			fprintf(stderr, "mpgts::open(%u): getsockname(%u is now %u) failed: %d %s\n", i,
				(unsigned) sizeof(sin), (unsigned) sinlen, errno, strerror(errno));
			return 1;
		}
		udp_port[i] = ntohs(sin.sin_port);
	}

	if (pthread_create(&tsth, 0 /*attr*/, thread_wrapper, this)) {
		fprintf(stderr, "mpgts::open(): pthread_create failed: %d %s\n", errno, strerror(errno));
		tsth = 0;
		return 1;
	}
	return 0;
}

void mpgts::close()
{
	tun.close();
	if (tsth) {
		void * rv;
		if (pthread_cancel(tsth)) {
			fprintf(stderr, "mpgts::close(): pthread_cancel failed: %d %s\n", errno, strerror(errno));
		} else if (pthread_join(tsth, &rv)) {
			fprintf(stderr, "mpgts::close(): pthread_join failed: %d %s\n", errno, strerror(errno));
		}
		tsth = 0;
	}
	unsigned i;
	for (i = 0; i < 2; i++) if (udp_sock[i] != -1) {
		::close(udp_sock[i]);
		udp_sock[i] = -1;
	}
}

int mpgts::start_ts(u8 ch)
{
	if (ch >= tuner::NUM_CHANNELS) {
		fprintf(stderr, "mpgts::start_ts(%u) invalid\n", ch);
		return 1;
	}

	// reset all pkt state
	want_reset[ch]++;
	unsigned i;
	for (i = 0; i < 3; i++) {
		if (!want_reset[ch]) {	// wait for thread_main() to notice want_reset
			break;
		}
		usleep(100000);
	}
	if (i >= 3) {
		fprintf(stderr, "failed to signal want_reset%u\n", ch);
		return 1;
	}

	atsc[ch].tvch = tun.get_freq(ch);

	return tun.start_ts(ch, udp_port[ch]);
}
