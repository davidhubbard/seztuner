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

#include "iface.h"

namespace tuner_ns {

class mpgts;
class socket {
protected:
	u8 mac[6];
	u32 ip, myip;
	int sock;
	char ipstr[128 - sizeof(sock) - sizeof(ip) - sizeof(mac)];

	inline void maccopy4(u32 * dst, const u32 * src) { *dst = *src; }
public:
	socket(u32 ip_, const u8 * mac_, u32 myip_)
	{
		maccopy4((u32 *) mac, (const u32 *) mac_);
		mac[4] = mac_[4];
		mac[5] = mac_[5];
		ip = ip_;
		myip = myip_;
		sock = -1;
		ipstr[0] = 0;
	}

	const u8 * get_mac() const { return mac; }
	u32 get_ip() const { return ip; }
	u32 get_myip() const { return myip; }

	int open();
	int read(u8 * pkt, size_t * pktlen);
	int write(u8 * pkt, size_t pktlen, u8 pkt_type);
	void close();
	u8 * write_then_read(u8 * pkt, size_t pktlen, size_t * rxlen);

	int get_gpio(u32 * val);
	int set_gpio(u32 val);
	int get_demod8(u8 ch,  u32 addr, u8 * val);
	int set_demod8(u8 ch,  u32 addr, u8   val);
	int get_demod16(u8 ch, u32 addr, u32 * val);
	int set_demod16(u8 ch, u32 addr, u32   val);
	int get_demod24(u8 ch, u32 addr, u32 * val);
	int set_demod24(u8 ch, u32 addr, u32   val);
	int get_demod32(u8 ch, u32 addr, u32 * val);
	int set_demod32(u8 ch, u32 addr, u32   val);
	int get_demodN(u8 ch, u32 addr, u8 * arr, u8 len);
	int set_demodN(u8 ch, u32 addr, u8 * arr, u8 len);
	int reset_demod(u8 ch, unsigned reset_ms);	// a good choice for reset_ms is 20

	static mpgts * find(unsigned * num_tumers, unsigned debug = 0);
};

};
