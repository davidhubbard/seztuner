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

#include "socket.h"

namespace tuner_ns {

class mpgts;
class tuner {
protected:
	socket sock;
	u32 cur_gpio;

public:
	tuner(u32 ip_, const u8 * mac_, u32 myip_) : sock(ip_, mac_, myip_) {
		cur_gpio = 0;
		active_ant = nc;
		for (unsigned i = 0; i < NUM_CHANNELS; i++) {
			ch_state[i].i = off;
			ch_state[i].tvch = (unsigned) -1;
		}
	}

	const u8 * get_mac() const { return sock.get_mac(); }
	u32 get_ip() const { return sock.get_ip(); }
	u32 get_myip() const { return sock.get_myip(); }
	int open() { return sock.open(); }
	void close();
	static mpgts * find(unsigned * num_tuners, unsigned debug = 0) { return socket::find(num_tuners, debug); }

	int get_str(unsigned idx, char * buf, u8 len);
	int refresh_gpio() { return sock.get_gpio(&cur_gpio); }
	int init();

	enum tuner_constants {
		NUM_CHANNELS = 2,	// one tuner can receive 2 channels simultaneously
		TVCH_MIN = 2,
		TVCH_MAX = 51,
	};

	enum tuner_operating_mode {
		QAM64 = 0,	// EU DVB-T http://en.wikipedia.org/wiki/IQ_Modulation#Quantized_QAM
		QAM256 = 1,	// North American Cable, EU DVB-C
		VSB = 3,	// North American ATSC http://en.wikipedia.org/wiki/Single-sideband_modulation#Vestigial_sideband_.28VSB.29
	};

	enum tuner_antennas {
		nc,	// tuner can be disconnected from all antennas
		ant1,	// sezmi custom antenna connector on "right"
		ant2,	// sezmi custom antenna connector on "left" (next to 6VDC jack)
		coax,	// standard "F connector" for coax to external TV antenna (aka "external antenna" in this class)
	};

	static const u32 ch_freq[TVCH_MAX - TVCH_MIN + 1];

protected:
	// TODO: change this to public after QAM has been tested
	int set_modulation(u8 ch, tuner_operating_mode mode);

	enum tuner_amp_input {
		off = 0,
		vhf1,	// VHF using antenna #1
		vhf2,	// VHF using antenna #2
		uhf1,	// UHF using antenna #1
		uhf2,	// UHF using antenna #2
		external,	// external antenna
		TUNER_AMP_INPUT_MAX
	};

	int set_amp(u8 ch, tuner_amp_input state);

	struct ch_state_st {
		tuner_amp_input i;
		unsigned tvch;
	};
	tuner_antennas active_ant;
	ch_state_st ch_state[NUM_CHANNELS];

public:
	tuner_antennas get_antenna() const { return active_ant; };

	// get_freq() does not actually return a value in MHz, it returns a TV channel (so 7==channel 7 / 177MHz)
	// returns (unsigned) -1 if this amp is off
	unsigned get_freq(u8 ch) const {
		if (ch >= NUM_CHANNELS) return 0;
		if (ch_state[ch].i == off) return (unsigned) -1;
		return ch_state[ch].tvch;
	}

	// must call scan() or set_antenna() with ant!=nc before calling set_freq()
	// set_antenna(nc) will set both amps off, that will likely kill any ongoing reception
	int set_antenna(tuner_antennas ant);

	typedef void (* scan_cb)(void * ctx, unsigned idx, unsigned max);

	// must call scan() or set_antenna() before calling set_freq()
	// scan() uses all DT3305 to scan in parallel - any ongoing reception will be stopped
	// scan() will take longer if active_ant==nc but it will autodetect the antenna
	// scan() will take longest if running inside a faraday cage (absolutely no signals found at all)
	// scan() will typically work faster with cr_ms of 0, the default is the recommended demod reset delay,
	//        and larger values make the scan even more sensitive. But if the channel does not appear with cr_ms == 0
	//        there is a good chance the channel is too weak to lock anyway
	//
	// scan() steps:
	// 1. pick a temporary antenna if active_ant==nc
	// 2. scan all channels
	// 3. if no signal is detected and this was a temporary antenna, try another antenna
	int scan(unsigned * n_ch, unsigned ** chlist, scan_cb cb = 0, void * ctx = 0, unsigned cr_ms = 20);

	// tvch must be >= TVCH_MIN and <= TVCH_MAX or (unsigned) -1 (turns the amp off)
	int set_freq(u8 ch, unsigned tvch, unsigned reset_ms = 20);

	// read signal strength
	int get_mse(u8 ch, u8 * status, u32 * ptmse, u32 * eqmse);

	// start streaming MPG Transport Stream to specified udp port, NUM_CHANNELS streams max
	int start_ts(u8 ch, unsigned udp_port);
	int stop_ts(u8 ch);
};

};
