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
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include "tuner.h"

using namespace tuner_ns;

void tuner::close()
{
	set_antenna(nc);
	sock.close();
}

int tuner::get_str(unsigned idx, char * buf, u8 len)
{
	if (idx > 2) {
		fprintf(stderr, "tuner::get_str(%u) invalid index\n", idx);	// the max is just a guess but all strings above it return FF
		return 1;
	}

	u8 pkt[] = {
			0,0,0,0,	// header
			0x0f, 0xf3,	// CPU bus (0x0ff2), read (| 1)
			len,  1,	// get string
			(u8) (idx + 1),
			0,0,0,0,	// CRC
		};
	size_t rxlen = len;
	u8 * rx = sock.write_then_read(pkt, sizeof(pkt), &rxlen);
	if (!rx) return 1;
	rxlen -= 4;
	memcpy(buf, &rx[4], rxlen);
	free(rx);
	if (rxlen < len) buf[rxlen] = 0;	// string should already have null but rxlen is not returned
	return 0;
}

int tuner::init()
{
	char ver[12];
	if (get_str(0, ver, sizeof(ver))) return 1;
	unsigned long v;
	if (sscanf(ver, "%lu", &v) != 1) {
		fprintf(stderr, "tuner::init(): unable to parse version \"%s\"\n", ver);
		return 1;
	}
	if (v <= 20081010lu) {
		fprintf(stderr, "tuner::init(): version %s uses different GPIOs, not safe to proceed.\n", ver);
		return 1;
	}

	// VSB modulation for North American ATSC broadcast
	for (v = 0; v < NUM_CHANNELS; v++) {
		ch_state[v].i = vhf1;	// fake a value of vhf1 so set_amp() thinks there was a change
		if (set_amp(v, off)) return 1;
		if (set_modulation(v, VSB)) return 1;
	}
	return 0;
}

struct demod_init8 {
	u32 addr;
	u8 val;
};

int tuner::set_modulation(u8 ch, tuner_operating_mode mode)
{
	if (ch >= NUM_CHANNELS) {
		fprintf(stderr, "tuner::set_modulation(%u, %u) invalid channel\n", ch, (unsigned) mode);
		return 1;
	}

	u8 b;
	// from linux kernel: verify it is really an lgdt3305
	if (sock.get_demod8(ch, 1, &b)) return 1;
	if (!b) {
		fprintf(stderr, "GEN CTRL 2 should not ever be 00: hardware error?\n");
		return 1;
	}
	if (sock.set_demod8(ch, 0x808, 0x80)) return 1;	// some undocumented BERT register?
	if (sock.get_demod8(ch, 0x808, &b)) return 1;
	if (b != 0x80) {
		fprintf(stderr, "BERT reg = %02x: hardware error?\n", b);
		return 1;
	}
	if (sock.set_demod8(ch, 0x808, 0)) return 1;	// some undocumented BERT register?

	if (sock.get_demod8(ch, 0, &b)) return 1;
	b &= ~3;
	b |= (u8) mode;
	if (sock.set_demod8(ch, 0, b)) return 1;

	// TODO: optimize this
	// writes to consecutive addresses can be grouped
	static const demod_init8 vsb1[] = {
			{  0x0d, 0x63 },	// enable digital SAW filter
			{  0x0e, 0x02 },	// sync CCR (confidential count register)
			{  0x12, 0x32 },	// DAGCREF (2 bytes)
			{  0x13, 0xc4 },	//         (2 bytes)
			{ 0x106, 0 },	// set IF, linux uses 0x4f0cacba unconditionally
			{ 0x107, 0 },	// but the default (0) apparently matches the TUA6034 - uncertain
			{ 0x108, 0 },	//
			{ 0x109, 0 },	//
			{ 0x112, 0x17 },	// EPHNTH output threshold for PED control (VSB carrier recovery)
			{ 0x113, 0x15 },	// GCONTH1 ave PED low threshold for PED control
			{ 0x114, 0x18 },	// GCONTH2 ave PED mid threshold for PED control
			{ 0x115, 0xff },	// GCONTH3 ave PED hi threshold for PED control
			{ 0x116, 0x3c },	// DMSELOWTH set threshold for demod_snr_low_resolution to 11dB
			{ 0x214, 0x27 },	// GSAUTOSL aka TRBW: do timing recovery at 1/2 bandwidth
			{ 0x424, 0x8d },	// CST_THD
			{ 0x427, 0x12 },	// EQCON_THD (2 bytes)
			{ 0x428, 0x4f },	//           (2 bytes)
			{ 0x302, 0x04 },	// REFD (2 bytes) set RF AGC loop delay, 12 bit signed value
			{ 0x303, 0xc0 },	//      (2 bytes)
			{ 0x306, 0x80 },	// RF AGC loop filter bw
			{ 0x307, 0x00 },	//
			{ 0x308, 0x80 },	// IFBW (2 bytes) since LOCKDTEN=0, only high 4 bits (0xf0) set AGC loop bandwidth (8: 2^8 gain)
			{ 0x309, 0x00 },	//      (2 bytes) if LOCKDTEN were 1, AGC lock detector would cycle through 4 steps starting with hi 4 bits
			{ 0x30c, 0x31 },	// AGC loop bandwidth (8x, no change); auto AGC ref (no change); set DC remover bandwidth 1/4x
			{ 0x30d, 0x00 },	// **added** make sure AGC loops are enabled
			{ 0x30e, 0x1c },	// turn off IN_AGC_BY (enable inner AGC loop); disable NSEN (no signal detector for QAM)
			{ 0x314, 0xe1 },	// turn off LOCKDTEN to finally disable all QAM circuits
		};

	unsigned i;
	switch (mode) {
	case VSB:
		for (i = 0; i < sizeof(vsb1)/sizeof(vsb1[0]); i++) {
			if (sock.set_demod8(ch, vsb1[i].addr, vsb1[i].val)) {
				fprintf(stderr, "set_demod8 failed for ch=%u vsb1[%u]\n", ch, i);
				return 1;
			}
		}
		if (sock.reset_demod(ch, 20)) return 1;
		if (sock.get_demod8(ch, 0x50e, &b)) {	// 0x50e: transport interface
			fprintf(stderr, "get_demod8 failed for ch=%u MPEGserial\n", ch);
			return 1;
		}
		b |= 0x20;	// configure serial output, bits are sent serially to Ubicom CPU on TPDATA0 line
		if (sock.set_demod8(ch, 0x50e, b)) {
			fprintf(stderr, "set_demod8 failed for ch=%u MPEGserial\n", ch);
			return 1;
		}
		if (sock.reset_demod(ch, 20)) return 1;
		break;

	default:
		fprintf(stderr, "tuner::set_modulation(%u) not implemented yet\n", (unsigned) mode);
		break;
	}
	return 0;
}

// Ubicom CPU GPIO:
#define GPIO_80F0 (0x80f0)

int tuner::set_amp(u8 ch, tuner_amp_input state)
{
	if (ch >= NUM_CHANNELS) {
		fprintf(stderr, "tuner::set_amp(%u, %u) invalid channel\n", ch, (unsigned) state);
		return 1;
	}
	if (state == ch_state[ch].i) return 0;

	cur_gpio &= GPIO_80F0;	// clear all amp settings

	tuner_amp_input update[2] = { ch_state[0].i, ch_state[1].i };
	update[ch] = state;	// update[] holds the new state, but it does not get written to ch_state[] yet

	// turn on an amp for any frequencies that are going to be received
	// e.g. if any DT3035 needs vhf1, both DT3035 0 and 1 get their vhf1 amp turned on
	static const u32 amp_gpio[] = {
			0,	// off
			1,	// vhf1
			4,	// vhf2
			2,	// uhf1
			8,	// uhf2
			0,	// external
		};
	unsigned i;
	for (i = 0; i < NUM_CHANNELS; i++) cur_gpio |= update[i];

	// configure tuner filter
	static const u32 filter_ch0[] = {
			4,	// off
			3,	// vhf1
			1,	// vhf2
			5,	// uhf1
			2,	// uhf2
			0,	// external
		};
	cur_gpio |= filter_ch0[update[0]] << 8;

	static const u32 filter_ch1[] = {
			0,	// off
			3,	// vhf1
			1,	// vhf2
			4,	// uhf1
			2,	// uhf2
			5,	// external
		};
	cur_gpio |= filter_ch1[update[1]] << 11;

	if (sock.set_gpio(cur_gpio)) return 1;

	// since sock.set_gpio() succeeded, write the final value to ch_state
	ch_state[ch].i = state;

	{
		// these will trigger a compiler error if any of the above arrays are sized wrong
		u32 amp_gpio_size_check1[(int) (sizeof(amp_gpio)/sizeof(amp_gpio[0]) - TUNER_AMP_INPUT_MAX)];
		u32 amp_gpio_size_check2[(int) (TUNER_AMP_INPUT_MAX - sizeof(amp_gpio)/sizeof(amp_gpio[0]))];
		u32 filter_ch0_size_check1[(int) (sizeof(filter_ch0)/sizeof(filter_ch0[0]) - TUNER_AMP_INPUT_MAX)];
		u32 filter_ch0_size_check2[(int) (TUNER_AMP_INPUT_MAX - sizeof(filter_ch0)/sizeof(filter_ch0[0]))];
		u32 filter_ch1_size_check1[(int) (sizeof(filter_ch1)/sizeof(filter_ch1[0]) - TUNER_AMP_INPUT_MAX)];
		u32 filter_ch1_size_check2[(int) (TUNER_AMP_INPUT_MAX - sizeof(filter_ch1)/sizeof(filter_ch1[0]))];
		(void) amp_gpio_size_check1; (void) amp_gpio_size_check2;
		(void) filter_ch0_size_check1; (void) filter_ch0_size_check2;
		(void) filter_ch1_size_check1; (void) filter_ch1_size_check2;
	}
	return 0;
}

int tuner::set_antenna(tuner_antennas ant)
{
	active_ant = ant;

	// apply active_ant to both DT3305 if they are tuned to a channel
	for (unsigned ch = 0; ch < NUM_CHANNELS; ch++) {
		tuner_amp_input tai = ch_state[ch].i;
		if (tai == vhf2) tai = vhf1;
		else if (tai == uhf2) tai = uhf1;
		else if (tai == off) continue;
		if (ant == nc) {
			if (set_amp(ch, off)) return 1;
			continue;
		}
		if (ant == coax) {
			if (set_amp(ch, external)) return 1;
			continue;
		}
		if (tai == external) {
			// recalculate the correct tai between vhf1,vhf2,uhf1,uhf2
			if (set_freq(ch, ch_state[ch].tvch)) return 1;
			continue;
		}

		if (ant == ant2) tai = (tuner_amp_input) ((u32) tai + 1);

		if (ch_state[ch].tvch < TVCH_MIN || ch_state[ch].tvch > TVCH_MAX) {
			fprintf(stderr, "tuner::set_antenna(%u) Warn: ch=%u has tvch=%x, amp cannot be set\n", ant, ch, ch_state[ch].tvch);
		} else {
			if (set_amp(ch, tai)) return 1;
		}
	}
	return 0;
}

int tuner::set_freq(u8 ch, unsigned tvch, unsigned reset_ms /*= 20*/)
{
	if (ch >= NUM_CHANNELS || ((tvch < TVCH_MIN || tvch > TVCH_MAX) && tvch != (unsigned) -1)) {
		fprintf(stderr, "tuner::set_freq(%u, %u) invalid\n", ch, tvch);
		return 1;
	}
	if (active_ant == nc) {
		fprintf(stderr, "tuner::set_freq(%u, %u) cannot be called before set_antenna()\n", ch, tvch);
		return 1;
	}
	if (tvch == (unsigned) -1) return set_amp(ch, off);
	if (!ch_freq[tvch - TVCH_MIN]) {
		fprintf(stderr, "tuner::set_freq(%u, %u) LOGIC ERROR: ch_freq=0\n", ch, tvch);
		return 1;
	}

	u32 freq = ch_freq[tvch];
	tuner_amp_input tai;
	u8 bandswitch;
	if (freq < 158 /*MHz*/) {
		bandswitch = 1 << 0;	// vhf low band: P0 turns on a GPIO + vhf filter, see datasheet
		tai = vhf1;
	} else if (freq < 452 /*MHz*/) {
		bandswitch = 1 << 1;	// vhf hi band: P1 turns on a GPIO + vhf filter, see datasheet
		tai = vhf1;
	} else if (freq < 862 /*MHz*/) {
		bandswitch = 1 << 2;	// uhf band: P2 turns on a GPIO (choosing P3 kills all uhf reception)
		tai = uhf1;
	} else {
		fprintf(stderr, "tuner::set_freq(%u, %u): %u MHz is out of range\n", ch, tvch, freq);
		return 1;
	}
	if (active_ant == ant2) tai = (tuner_amp_input) ((u32) tai + 1);
	else if (active_ant == coax) tai = external;

	if (set_amp(ch, tai)) return 1;

	// freq is in MHz - need a PLL setting in units of 62.5kHz (1/16 MHz)
	// so multiply pll * 16 or d << 4 to get PLL setting
	u32 pll = (freq << 4) + 704;

	// LG Infineon TUA6034 3-Band Digital TV Tuner IC "TAIFUN"
	u8 pkt[] = {
			0,0,0,0,	// header
			ch, 0xc2,	// Tuner i2c address (ch*256 + 0xc2), write (| 0)
			(u8) ((pll >> 8) & 0x7f), (u8) pll,	// divider
			0x86,		// control register 1
			bandswitch,
			0x50,		// AGC register
			0,0,0,0,	// CRC
		};
	size_t rxlen = 8;
	u8 * rx = sock.write_then_read(pkt, sizeof(pkt), &rxlen);
	if (!rx) return 1;
	if (rxlen != 4) {
		fprintf(stderr, "tuner::set_freq(%u, %u) write fault\n", ch, tvch);
		if (set_amp(ch, off)) fprintf(stderr, "tuner::set_freq(%u, %u) failed to disable amp after fault\n", ch, tvch);
		return 1;
	}
	free(rx);
	if (reset_ms) {
		if (sock.reset_demod(ch, reset_ms)) return 1;
	}
	ch_state[ch].tvch = tvch;
	return 0;
}

static int tuner_scan_cmp(const void * p1, const void * p2)
{
	return *(const unsigned *) p1 - *(const unsigned *) p2;
}

#define CH_STEP (2)
static void tuner_scan_call_cb(tuner::scan_cb cb, void * ctx, unsigned i, tuner::tuner_antennas ant,
	unsigned ant_valid, unsigned find_use, unsigned * find)
{
	if (!cb) return;
	static const unsigned n_ch_freq = (sizeof(tuner::ch_freq)/sizeof(tuner::ch_freq[0]) + 1)/CH_STEP;

	// i goes 1,5,9 (odds) then 2,6,10 (evens) - convert that to a sequential count
	if (i) {
		i--;
		i = i/(2*CH_STEP) + (i & 1)*((n_ch_freq + 1)/2);
		i++;
	}

	unsigned max = n_ch_freq;
	if (!ant_valid) {
		max *= 3;
		if (find_use >= 3) {
			// found channels, interpolate progress for the rest of the scan
			unsigned found_i = find[find_use - 1];
			found_i = found_i/(2*CH_STEP) + (found_i & 1)*((n_ch_freq + 1)/2);

			unsigned found_i3 = found_i;
			if (ant == tuner::ant2) found_i3 += n_ch_freq;
			else if (ant == tuner::coax) found_i3 += n_ch_freq*2;

			if (found_i < n_ch_freq)
				i = (i - found_i)*(n_ch_freq*3 - found_i3 + 1)/(n_ch_freq - found_i + 1) + found_i;
		}
		if (ant == tuner::ant2) i += n_ch_freq;
		else if (ant == tuner::coax) i += n_ch_freq*2;
	}

	cb(ctx, i, max + 1);
}

int tuner::scan(unsigned * n_ch, unsigned ** chlist, scan_cb cb /*= 0*/, void * ctx /*= 0*/, unsigned cr_ms /*= 20*/)
{
	unsigned ant_valid;
	if (get_antenna() == nc) {
		ant_valid = 0;	// temporary antenna
		if (ch_state[0].i != off || ch_state[1].i != off) {
			fprintf(stderr, "tuner::scan: channel amps are not off: %u %u\n", ch_state[0].i, ch_state[1].i);
			return 1;
		}
		if (set_antenna(ant1)) return 1;
		if (cr_ms < 20) cr_ms = 20;	// must spen at least 20ms to correctly detect antenna
	} else {
		ant_valid = 1;	// do not change antennas
	}

	static const unsigned n_ch_freq = sizeof(ch_freq)/sizeof(ch_freq[0]);
	unsigned find_use = 0;
	unsigned * find = (typeof(find)) malloc(sizeof(*find) * n_ch_freq);
	if (!find) {
		fprintf(stderr, "tuner::scan: malloc failed\n");
		return 1;
	}
	tuner_scan_call_cb(cb, ctx, 0, get_antenna(), ant_valid, find_use, find);

	// register 0x12a is not documented but the LG DT3305 example and app notes both suggest
	// clearing bit 0x20 to disable the DT3305 frequency modulation
	// this isolates carrier recovery for a more accurate result
	unsigned i, j;
	u8 old12a[NUM_CHANNELS];
	for (j = 0; j < NUM_CHANNELS; j++) {
		if (sock.get_demod8(j, 0x12a, &old12a[j]) ||
			sock.set_demod8(j, 0x12a, old12a[j] & ~0x20))
		{
			free(find);
			return 1;
		}
	}

	for (;;) {
		// scan in parallel
		for (i = 0;; i += NUM_CHANNELS*2) {
			if (i >= n_ch_freq) {		// scan channels interleaved (CH_STEP): first evens, then odds
				if (i & 1) break;	// i is odd, done scanning ... but results still need to be sorted
				i = 1;			// i is even, restart with odds
			}

			tuner_scan_call_cb(cb, ctx, i + 1, get_antenna(), ant_valid, find_use, find);
			for (j = 0; j < NUM_CHANNELS; j++) if (i + j*CH_STEP < n_ch_freq) {
				if (!ch_freq[i + j*CH_STEP]) {
					fprintf(stderr, "tuner::scan() i=%u got freq=0\n", i + j*CH_STEP);
					continue;
				}

				if (set_freq(j, i + j*CH_STEP + TVCH_MIN, cr_ms <= 20 ? cr_ms : 0)) goto fail;
			}

			unsigned wait_tally = cr_ms;
			if (wait_tally > 20) usleep((wait_tally - 20) * 1000);
			u8 b[NUM_CHANNELS];
			for (j = 0; j < NUM_CHANNELS; j++) if (i + j*CH_STEP < n_ch_freq) {
				if (sock.get_demod8(j, 0x11d, &b[j])) goto fail;	// carrier recovery lock
				if (!(b[j] & 0x80)) continue;
				find[find_use++] = ch_state[j].tvch;
			}
		}

		// done scanning: are there any channels?
		if (find_use >= 3) break;	// found channels
		if (ant_valid) break;		// cannot try switching antennas
		if (get_antenna() == coax) break;	// already tried all antennas

		for (j = 0; j < NUM_CHANNELS; j++) if (set_amp(j, off)) return 1;
		if (set_antenna((tuner_antennas) (get_antenna() + 1))) {
			free(find);
			return 1;
		}
		//fprintf(stderr, "try antenna %u\n", get_antenna());
	}

	for (j = 0; j < NUM_CHANNELS; j++)
		if (sock.set_demod8(j, 0x12a, old12a[j])) {
			free(find);
			return 1;
		}

	qsort(find, find_use, sizeof(find[0]), tuner_scan_cmp);	// sort list
	*n_ch = find_use;
	*chlist = find;
	return 0;

fail:
	free(find);
	for (j = 0; j < NUM_CHANNELS; j++) sock.set_demod8(j, 0x12a, old12a[j]);
	return 1;
}

int tuner::get_mse(u8 ch, u8 * status, u32 * ptmse, u32 * eqmse)
{
	u8 lock;
	if (sock.get_demod8(ch, 0x11d, &lock)) return 1;	// carrier recovery lock
	if (!(lock & 0x80)) {
		*status = 0;
		*ptmse = 0xfffff;
		*eqmse = 0xfffff;	// technically 7ffff
		return 0;
	}

	if (sock.get_demod8(ch, 3, &lock)) return 1;	// register 3: general status
	*status =  1 |
		(((lock & 8) >> 2) ^ 2) |	// has lock (nlock=="inlock")
		(lock & 4) |			// has sync lock
		((lock & 1) << 3) |		// snr above tov
		((lock & 2) << 3);		// has viterbi ("fec ok")

	// 24-bit value at 0x413: equalizer mean square error (mse) for VSB
	// 24-bit value at 0x417: phase tracker mean square error (mse) for VSB
	// ... ignored: 24-bit value at 0x118: carrier recovery frequency offset
	//
	// ptmse and eqmse can be read in a single operation
	u8 msebuf[8];
	if (sock.get_demodN(ch, 0x413, msebuf, sizeof(msebuf))) return 1;

	*ptmse = ((u32) msebuf[4] << 16) | ((u32) msebuf[5] << 8) | msebuf[6];
	*eqmse = ((u32) msebuf[0] << 16) | ((u32) msebuf[1] << 8) | msebuf[2];
	return 0;
}

int tuner::start_ts(u8 ch, unsigned udp_port)
{
	size_t rxlen;
	u8 * rx;
	if (ch) {
		u8 pkt[] = {
				0,0,0,0,	// header
				0x0f, 0xf2,	// CPU bus (0x0ff2), write (| 0)
				6,		// (3) set output ("PID bypass"; note: can also get PID bypass: tx {6, ch} rx 1 byte=output)
				ch,		// tuner     note: PID remap: tx {5, ch, up to 32*{pidhi, pidlo, (output << 5)|pidhi, pidlo}}
				ch,		// output    note: get PID remap: tx {5, ch} ask for rx of 32*4, end-of-list will be all ff
				0,0,0,0,	// CRC
			};
		rxlen = 8;
		rx = sock.write_then_read(pkt, sizeof(pkt), &rxlen);
		if (!rx) return 1;
		free(rx);
		if (rxlen != 4) {
			fprintf(stderr, "start_ts(): output rx %zu\n", rxlen);
			return 1;
		}
	}

	u32 ip = ntohl(sock.get_myip());
	u8 pkt[] = {
			0,0,0,0,	// header
			0x0f, 0xf2,	// CPU bus (0x0ff2), write (| 0)
			3, ch,		// (3) set UDP destination, output (not tuner actually, which is why "PID bypass" is needed)
			(u8) (ip >> 24),
			(u8) (ip >> 16),
			(u8) (ip >> 8),
			(u8) ip,
			(u8) (udp_port >> 8), (u8) udp_port,	// one of 0x138a or 0x138c or any may work
			0,0,0,0,	// CRC
		};
	rxlen = 8;
	rx = sock.write_then_read(pkt, sizeof(pkt), &rxlen);
	if (!rx) return 1;
	free(rx);
	if (rxlen != 4) {
		fprintf(stderr, "start_ts(): dest rx %zu\n", rxlen);
		return 1;
	}
	return 0;
}

int tuner::stop_ts(u8 ch)
{
	u8 pkt[] = {
			0,0,0,0,	// header
			0x0f, 0xf2,	// CPU bus (0x0ff2), write (| 0)
			3, ch,		// (3) set UDP destination
			0,0,0,0,	// IP:   0
			0,0,		// Port: 0
			0,0,0,0,	// CRC
		};
	size_t rxlen = 8;
	u8 * rx = sock.write_then_read(pkt, sizeof(pkt), &rxlen);
	if (!rx) return 1;
	free(rx);
	if (rxlen != 4) {
		fprintf(stderr, "stop_ts(): dest rx %zu\n", rxlen);
		return 1;
	}
	return 0;
}

const u32 tuner::ch_freq[] = {
	57,	// channel 2
	63,	// channel 3
	69,	// channel 4
	79,	// channel 5 may be converted to FM broadcast at some point in the future
	85,	// channel 6 may be converted to FM broadcast at some point in the future
	177,	// channel 7
	183,	// channel 8
	189,	// channel 9
	195,	// channel 10
	201,	// channel 11
	207,	// channel 12
	213,	// channel 13
	473,	// channel 14
	479,	// channel 15
	485,	// channel 16
	491,	// channel 17
	497,	// channel 18
	503,	// channel 19
	509,	// channel 20
	515,	// channel 21
	521,	// channel 22
	527,	// channel 23
	533,	// channel 24
	539,	// channel 25
	545,	// channel 26
	551,	// channel 27
	557,	// channel 28
	563,	// channel 29
	569,	// channel 30
	575,	// channel 31
	581,	// channel 32
	587,	// channel 33
	593,	// channel 34
	599,	// channel 35
	605,	// channel 36
	611,	// channel 37 reserved for radio astronomy
	617,	// channel 38
	623,	// channel 39
	629,	// channel 40
	635,	// channel 41
	641,	// channel 42
	647,	// channel 43
	653,	// channel 44
	659,	// channel 45
	665,	// channel 46
	671,	// channel 47
	677,	// channel 48
	683,	// channel 49
	689,	// channel 50
	695,	// channel 51 no new stations will on this channel: http://hraunfoss.fcc.gov/edocs_public/attachmatch/DA-11-1428A1_Rcd.pdf

/*
these channels were used for TV before http://en.wikipedia.org/wiki/United_States_2008_wireless_spectrum_auction - they are no longer used
	701,	// channel 52
	707,	// channel 53
	713,	// channel 54
	719,	// channel 55
	725,	// channel 56
	731,	// channel 57
	737,	// channel 58
	743,	// channel 59
	749,	// channel 60
	755,	// channel 61
	761,	// channel 62
	767,	// channel 63 public safety
	773,	// channel 64 public safety
	779,	// channel 65
	785,	// channel 66
	791,	// channel 67
	797,	// channel 68
	803,	// channel 69
*/
};
