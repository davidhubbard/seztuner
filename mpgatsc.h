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

namespace tuner_ns {

class mpgatsc {
protected:
	u8 * pktlist[0x2000];	// one pkt per PID
	u32 pktlen[sizeof(pktlist)/sizeof(pktlist[0])];

	u32 pat_curver, pat_tsid, pmt_pid, pmt_curver, pcr_pid, mgt_curver;
	volatile u32 vct_curver;
	unsigned pat_use, pat_max;
	unsigned * pat;
	void * iconv_hnd;
	char * vctstr;
	char dumpfilename[256];
	void * dumpfile;

	int parse_mgt(u8 * pkt, u32 pos, u32 len);
	int parse_vct(u8 * pkt, u32 pos, u32 len, const char * tblname);
	int parse_descriptors(u8 * pkt, u32 pos, u32 len, const char * tblname);
	int parse_tbl(u8 * pkt, u32 len, u32 pid);

public:
	mpgatsc() {
		for (unsigned i = 0; i < sizeof(pktlist)/sizeof(pktlist[0]); i++) {
			pktlist[i] = 0;
			pktlen[i] = 0;
		}

		pat_curver = 32;	// valid values are 0-31, 32 indicates no pat found yet
		pat_tsid = 0;
		pmt_pid = 0;
		pmt_curver = 32;	// valid values are 0-31, 32 indicates no pmt found yet
		pcr_pid = 0;
		mgt_curver = 32;	// valid values are 0-31, 32 indicates no mgt found yet
		vct_curver = 32;	// valid values are 0-31, 32 indicates no mgt found yet
		pat_use = 0;
		pat_max = 0;
		pat = 0;
		iconv_hnd = 0;
		vctstr = 0;
		dumpfilename[0] = 0;
		dumpfile = 0;
		tvch = 0;
	}

	~mpgatsc();

	int init();

	int thread_demux(u8 pkt[188]);

	const char * get_vct() const { return vctstr; }

	int open_dump(const char * filename);

	unsigned tvch;
};

}

void dump_pkt(u8 * pkt, u32 len);
