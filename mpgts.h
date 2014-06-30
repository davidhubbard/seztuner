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

#include "tuner.h"
#include "mpgatsc.h"

typedef unsigned long int pthread_t;

namespace tuner_ns {

class mpgts {
protected:
	tuner tun;
	mpgatsc atsc[2];
	int udp_sock[2];
	unsigned udp_port[2];
	volatile int want_reset[2];
	pthread_t tsth;

	static void * thread_wrapper(void * arg);
	void * thread_main();

public:
	mpgts(u32 ip_, const u8 * mac_, u32 myip_) : tun(ip_, mac_, myip_) {
		udp_sock[0] = -1;
		udp_sock[1] = -1;
		udp_port[0] = 0;
		udp_port[1] = 0;
		want_reset[0] = 0;
		want_reset[1] = 0;
		tsth = 0;
	}

	const u8 * get_mac() const { return tun.get_mac(); }
	u32 get_ip() const { return tun.get_ip(); }
	u32 get_myip() const { return tun.get_myip(); }
	int open();
	void close();
	static mpgts * find(unsigned * num_tuners, unsigned debug = 0) { return tuner::find(num_tuners, debug); }
	tuner::tuner_antennas get_antenna() const { return tun.get_antenna(); }
	unsigned get_freq(u8 ch) const { return tun.get_freq(ch); }
	int set_antenna(tuner::tuner_antennas ant) { return tun.set_antenna(ant); }
	int scan(unsigned * n_ch, unsigned ** chlist, tuner::scan_cb cb = 0, void * ctx = 0, unsigned cr_ms = 20) {
		return tun.scan(n_ch, chlist, cb, ctx, cr_ms);
	}
	int set_freq(u8 ch, unsigned tvch) { return tun.set_freq(ch, tvch); }
	int get_mse(u8 ch, u8 * status, u32 * ptmse, u32 * eqmse) { return tun.get_mse(ch, status, ptmse, eqmse); }
	int start_ts(u8 ch);
	int stop_ts(u8 ch) { return tun.stop_ts(ch); }
	const char * get_vct(u8 ch) { if (ch >= tuner::NUM_CHANNELS) return 0; return atsc[ch].get_vct(); }
	int open_dump(u8 ch, const char * filename) { if (ch >= tuner::NUM_CHANNELS) return 1; return atsc[ch].open_dump(filename); }

#if 0
	// TODO: these are private, exposed only for debugging. Delete this.
	int get_demod8(u8 ch,  u32 addr, u8 * val)  { return tun.get_demod8(ch, addr, val); }
	int set_demod8(u8 ch,  u32 addr, u8   val)  { return tun.set_demod8(ch, addr, val); }
	int get_demod16(u8 ch, u32 addr, u32 * val) { return tun.get_demod16(ch, addr, val); }
	int set_demod16(u8 ch, u32 addr, u32   val) { return tun.set_demod16(ch, addr, val); }
	int get_demod24(u8 ch, u32 addr, u32 * val) { return tun.get_demod24(ch, addr, val); }
	int set_demod24(u8 ch, u32 addr, u32   val) { return tun.set_demod24(ch, addr, val); }
	int get_demod32(u8 ch, u32 addr, u32 * val) { return tun.get_demod32(ch, addr, val); }
	int set_demod32(u8 ch, u32 addr, u32   val) { return tun.set_demod32(ch, addr, val); }
	int get_gpio(u32 * val) { return tun.get_gpio(val); }
	int set_gpio(u32 val) { return tun.set_gpio(val); }
#endif
};

};
