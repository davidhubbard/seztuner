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

#define _LARGEFILE64_SOURCE
#include <stdio.h>
#include <errno.h>
#include <iconv.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include "mpgts.h"

using namespace tuner_ns;

int mpgatsc::init()
{
	if (!iconv_hnd) {
		iconv_hnd = iconv_open("ASCII", "UTF-16");
		if (iconv_hnd == (iconv_t) -1) {
			fprintf(stderr, "iconv_open(ASCII, UTF-16) failed: %d %s\n", errno, strerror(errno));
			return 1;
		}
	}

	// may be receiving a completely new stream: reset all state
	unsigned i;
	for (i = 0; i < sizeof(pktlist)/sizeof(pktlist[0]); i++) {
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
	if (vctstr) {
		free(vctstr);
		vctstr = 0;
	}

	// only reset dumpfile if bytes have been written
	// otherwise, calling open_dump() right before start_ts() would result in dumpfile getting closed
	if (dumpfile && lseek64(fileno((FILE *) dumpfile), 0, SEEK_CUR) != 0) {
		dumpfilename[0] = 0;
		fclose((FILE *) dumpfile);
		dumpfile = 0;
	}

	return 0;
}

mpgatsc::~mpgatsc()
{
	if (vctstr) {
		free(vctstr);
		vctstr = 0;
	}
	if (pat) {
		free(pat);
		pat = 0;
		pat_use = 0;
		pat_max = 0;
	}
	for (unsigned i = 0; i < sizeof(pktlist)/sizeof(pktlist[0]); i++) {
		if (pktlist[i]) {
			free(pktlist[i]);
			pktlist[i] = 0;
			pktlen[i] = 0;
		}
	}
	if (iconv_hnd) {
		iconv_close(iconv_hnd);
		iconv_hnd = 0;
	}
	if (dumpfile) {
		fclose((FILE *) dumpfile);
		dumpfile = 0;
	}
}


void dump_pkt(u8 * pkt, u32 len)
{
	for (u32 i = 0; i < len; i++) fprintf(stderr, " %x=%02x", i, pkt[i]);
	fprintf(stderr, "\n");
}

static u32 mpeg2_crc(u8 * pkt, size_t len)
{
	u32 crc = (u32) -1;
	for (size_t i = 0; i < len; i++, pkt++) {
		u8 x = ((u8) (crc >> 24)) ^ *pkt;
		crc <<= 8;
		if (x & 0x01) crc ^= 0x04c11db7;	// poly << 0
		if (x & 0x02) crc ^= 0x09823b6e;	// poly << 1
		if (x & 0x04) crc ^= 0x130476dc;	// poly << 2
		if (x & 0x08) crc ^= 0x2608edb8;	// poly << 3
		if (x & 0x10) crc ^= 0x4c11db70;	// poly << 4
		if (x & 0x20) crc ^= 0x9823b6e0;	// poly << 5, bit 31 is set now
		if (x & 0x40) crc ^= 0x34867077;	// poly << 6 ^ poly
		if (x & 0x80) crc ^= 0x690ce0ee;	// poly << 7 ^ poly << 1
	}
	return crc;
}

static int mpg_parse_hdr(u8 * pkt, u32 len, u32 * pos_ptr, u32 * id, u32 * ver, const char * tblname, u8 tblid)
{
	u32 pos = *pos_ptr;
	if (pkt[pos] != tblid) {
		fprintf(stderr, "mpg: %s tbl id %02x, throw away\n", tblname, pkt[*pos_ptr]);
		return 1;
	}

	// section_length is pkt[pos + 1] and pkt[pos + 2], already parsed and passed in as 'len'

	// check CRC
	if (len < 4) {
		fprintf(stderr, "mpg: PAT section length %u invalid\n", len);
		return 1;
	}
	{
		u32 crc = ((u32) pkt[pos + len - 4] << 24) + ((u32) pkt[pos + len - 3] << 16) + ((u32) pkt[pos + len - 2] << 8) + pkt[pos + len - 1];
		if (crc != mpeg2_crc(pkt + pos, len - 4)) {
			fprintf(stderr, "mpg: %s crc %x is not %x\n", tblname, crc, mpeg2_crc(pkt + pos, len));
			return 1;
		}
	}
	pos += 3;
	*pos_ptr = pos;

	if (pkt[pos - 2] & 0x80) {
		// found a long header
		*id = ((u32) pkt[pos] << 8) + pkt[pos + 1];	// table_id_extension
		if (!(pkt[pos + 2] & 1)) {	// 0 == current_next_indicator means "next"
			*pos_ptr = 0;
			return 1;		// ignore it
		}
		if (*ver == 32) {	// no valid PAT found previously
			*ver = (pkt[pos + 2] & 0x2f) >> 1;
		} else {	// ignore unless the version number is valid
			u32 rx_ver = (pkt[pos + 2] & 0x2f) >> 1;
			if (rx_ver == *ver) {
				//fprintf(stderr, "%s: same old\n", tblname);
				*pos_ptr = 0;
				return 1;
			}
			if (rx_ver != ((*ver + 1) & 0x1f)) {
				fprintf(stderr, "mpg: %s ver %u, not %u, throw away\n", tblname, rx_ver, *ver);
				return 1;
			}
			*ver = rx_ver;
			fprintf(stderr, "mpg: new %s ver %u\n", tblname, *ver);
		}
		if (pkt[pos + 4]) {
			fprintf(stderr, "mpg: %s with max_section=%u, throw away\n", tblname, pkt[pos + 4]);
			return 1;
		}
		if (pkt[pos + 3]) {
			fprintf(stderr, "mpg: %s with max_section=0, invalid section=%u\n", tblname, pkt[pos + 3]);
			return 1;
		}
		if (pos + 5 > len) {
			fprintf(stderr, "mpg: %s section length %u invalid because long header\n", tblname, len);
			return 1;
		}
		pos += 5;
		*pos_ptr = pos;
	}
	if (pos + 4 > len) {
		fprintf(stderr, "mpg: %s empty, throw away\n", tblname);
		return 1;
	}
	return 0;
}

static int atsc_parse_hdr(u8 * pkt, u32 len, u32 * pos_ptr, u32 * ver, const char * tblname)
{
	u32 id = 0;
	if (mpg_parse_hdr(pkt, len, pos_ptr, &id, ver, tblname, pkt[*pos_ptr])) return 1;

	// check atsc_protocol_version
	if (pkt[*pos_ptr]) {
		fprintf(stderr, "atsc: %s tbl atsc_protocol_version=%x\n", tblname, pkt[*pos_ptr]);
		return 1;
	}
	(*pos_ptr)++;
	return 0;
}

int mpgatsc::parse_mgt(u8 * pkt, u32 len, u32 pos)
{
	u32 count = ((u32) pkt[pos] << 8) + pkt[pos + 1];
	pos += 2;

	u32 i, show_mgt = 1;
	for (i = 0; i < count; i++) {
		u32 tbltype = ((u32) pkt[pos] << 8) + pkt[pos + 1];
		pos += 2;
		u32 tblpid = (((u32) pkt[pos] << 8) + pkt[pos + 1]) & 0x1fff;
		pos += 2;
		u8 ver = pkt[pos] & 0x1f;
		(void) ver;
		pos++;
		u32 tbllen = ((u32) pkt[pos] << 24) + ((u32) pkt[pos] << 16) + ((u32) pkt[pos] << 8) + pkt[pos];
		(void) tbllen;
		pos += 4;
		u32 tbldesclen = (((u32) pkt[pos] << 8) + pkt[pos + 1]) & 0xfff;
		pos += 2;
		pos += tbldesclen;

		u32 unknown = 1;

		switch (tbltype >> 7) {
		case 0:
			if (tbltype < 4) {
				//case 0:	// TVCT current_next=1
				//case 1:	// TVCT current_next=0
				//case 2:	// CVCT current_next=1
				//case 3:	// CVCT current_next=0
				if (tblpid != 0x1ffb) fprintf(stderr, " pid not 1ffb?");
					else unknown = 0;
			} else if (tbltype < 6) {
				//case 4:	// Channel ECT
				//case 5:	// DCCSCT
				unknown = 0;
			} else {
				// 6-0x7f reserved per ATSC A65_2013
			}
			break;
		case 1:	//  0x80- 0xff reserved per ATSC A65_2013
		case 3:	// 0x180-0x1ff reserved per ATSC A65_2013
		case 5:	// 0x280-0x2ff reserved per ATSC A65_2013
			break;
		case 2:	// EIT
			unknown = 0;
			break;
		case 4:	// ETT
			unknown = 0;
			break;
		case 6:	// RRT, except 0x300 reserved
		case 7:	// RRT (0x301 - 0x3ff)
			unknown = 0;
			break;
		}

		if (unknown) {
			fprintf(stderr, "%s %x:pid=%x", show_mgt ? "mgt" : "", tbltype, tblpid);
			show_mgt = 0;
		}
		if (pos >= len) {
			fprintf(stderr, " overflow\n");
			return 1;
		}
	}
	if (!show_mgt) fprintf(stderr, "\n");
	return 0;
}

int mpgatsc::parse_vct(u8 * pkt, u32 len, u32 pos, const char * tblname)
{
	if (!vctstr) {
		vctstr = (typeof(vctstr)) malloc(65536);
		if (!vctstr) {
			fprintf(stderr, "mpgatsc::parse_vct: malloc failed\n");
			return 1;
		}
	}
	vctstr[0] = 0;

	unsigned debug = 0;
	u16 shortname[16];
	u32 shortname_len = strcmp(tblname, "SVCT") ? 7 /*TVCT, CVCT*/ : 16 /*SVCT*/;
	u32 count = pkt[pos];
	pos++;
	if (debug) fprintf(stderr, "%s:\n", tblname);

	u32 i;
	for (i = 0; i < count; i++) {
		u32 ch_no;
		for (ch_no = 0; ch_no < shortname_len; ch_no++)
			shortname[ch_no] = ((u32) pkt[pos + ch_no*sizeof(shortname[0])] << 8) + pkt[pos + ch_no*sizeof(shortname[0]) + 1];
		pos += shortname_len*sizeof(shortname[0]);
		ch_no = (((u32) pkt[pos] << 16) + ((u32) pkt[pos + 1] << 8) + pkt[pos + 2]) & 0xfffff;
		pos += 3;
		u8 modulation = pkt[pos];
		const char * modulation_str;
		char modulation_str_buf[32];
		switch (modulation) {
		case 0: modulation_str = "bad:0"; break;
		case 1: modulation_str = "analog"; break;
		case 2: modulation_str = "scte1"; break;
		case 3: modulation_str = "scte2"; break;
		case 4: modulation_str = ""; break;	// 8vsb is expected
		case 5: modulation_str = "16vsb"; break;
		default:
			if (modulation < 0x80) {
				snprintf(modulation_str_buf, sizeof(modulation_str_buf), "bad:%u", modulation);
			} else {
				snprintf(modulation_str_buf, sizeof(modulation_str_buf), "user:%x", modulation);
			}
			modulation_str = modulation_str_buf;
			break;
		}
		pos++;
		u32 carrier_hz = ((u32) pkt[pos] << 24) + ((u32) pkt[pos] << 16) + ((u32) pkt[pos] << 8) + pkt[pos];
		(void) carrier_hz;
		pos += 4;
		u32 ch_tsid = ((u32) pkt[pos] << 8) + pkt[pos + 1];
		(void) ch_tsid;
		pos += 2;
		u32 prog = ((u32) pkt[pos] << 8) + pkt[pos + 1];	// look up this in pat (pat[prog]) to get "base PID"
		pos += 2;
		u32 flags = ((u32) pkt[pos] << 8) + pkt[pos + 1];
		(void) flags;
		pos += 2;
		u32 src_id = ((u32) pkt[pos] << 8) + pkt[pos + 1];
		(void) src_id;
		pos += 2;

		char shortname_ascii[64];
		{
			char * src = (char *) shortname, * dst = shortname_ascii;
			size_t srclen = shortname_len*sizeof(shortname[0]), dstlen = sizeof(shortname_ascii) - 1;

			size_t icr = iconv(iconv_hnd, &src, &srclen, &dst, &dstlen);
			shortname_ascii[sizeof(shortname_ascii) - 1 - dstlen] = 0;
			if (icr == (size_t) -1 && dstlen < sizeof(shortname_ascii) - 4) strcat(shortname_ascii, "err");
		}

		char vline[256];
		snprintf(vline, sizeof(vline), " %2u  %u.%u%s%s \"%s\"", tvch, ch_no >> 10, ch_no & 1023, modulation_str[0] ? " " : "", modulation_str, shortname_ascii);

		if (debug) fprintf(stderr, "    dtv %s pat%x\n", vline, prog);
		strcat(vctstr, vline);
		strcat(vctstr, "\n");

		u32 tbldesclen = (((u32) pkt[pos] << 8) + pkt[pos + 1]) & 0x3ff;
		pos += 2;
		if (parse_descriptors(pkt, pos, tbldesclen, tblname)) return 1;
		pos += tbldesclen;
		if (pos > len) {
			fprintf(stderr, " overflow, need more sections\n");
			return 0;
		}
	}
	return 0;
}

int mpgatsc::parse_descriptors(u8 * pkt, u32 pos, u32 len, const char * tblname)
{
	(void) tblname;
	while (len >= 2) {
		u8 type = pkt[pos];
		u8 dlen = pkt[pos + 1];
		pos += 2;
		len -= 2;
		switch (type) {
		case 0xa0:	// extended channel name - uses "6.10 multiple string structure"
			break;
		default:	// must ignore unknown descriptors
			break;	// descriptor 0x80 is treated as unknown, but it is "stuffing" i.e. padding bytes
		}
		pos += dlen;
		len -= dlen;
	}
	if (len) {
		fprintf(stderr, "  descriptors len=%u ignored\n", len);
	}
	return 0;
}

int mpgatsc::parse_tbl(u8 * pkt, u32 len, u32 pid)
{
	u32 pos = 0, debug = 0;

	switch (pid) {
	case 0:	// parse PAT
	{	// see http://www.etherguidesystems.com/help/sdos/mpeg/syntax/tablesections/pat.aspx
		u32 id = 0;
		if (mpg_parse_hdr(pkt, len, &pos, &id, &pat_curver, "PAT", 0 /*PAT tblid*/)) return pos ? 1 : 0;
		if (id) pat_tsid = id;
		if (((pos + len) & 3) != 0) {
			fprintf(stderr, "parse_tbl: PAT section length %u invalid\n", len);
			return 1;
		}
		pat_use = 0;

		if (debug) fprintf(stderr, "PAT:%x", pat_tsid);
		u32 start_pos = pos;
		u32 count = (len - pos) >> 2;
		count--;	// remove 4 bytes of CRC at the end
		u32 i;
		for (i = 0; i < count; i++) {
			u32 prog = ((u32) pkt[pos] << 8) + pkt[pos + 1];
			pos += 2;
			u32 childpid = (((u32) pkt[pos] << 8) + pkt[pos + 1]) & 0x1fff;
			pos += 2;
			if (prog > pat_use) pat_use = prog;

			switch (prog) {
			case 3:	// PMP
				if (childpid) pmt_pid = childpid;
				break;
			default:
				if (debug) fprintf(stderr, " %x=%x%s", prog, childpid, (i == (count-1)) ? "\n" : "");
				break;
			}
		}
		if (!pmt_pid) fprintf(stderr, "Warn: PAT missing PMT\n");

		pat_use++;
		if (pat_use >= pat_max) {
			if (pat) {
				free(pat);
				pat = 0;
			}
			pat_max = pat_use;
		}
		if (!pat) {
			pat = (typeof(pat)) calloc(sizeof(*pat), pat_max);
			if (!pat) {
				fprintf(stderr, "parse_tbl: calloc pat failed\n");
				return 1;
			}
		}

		pos = start_pos;
		for (i = 0; i < count; i++) {
			u32 prog = ((u32) pkt[pos] << 8) + pkt[pos + 1];
			pos += 2;
			u32 childpid = (((u32) pkt[pos] << 8) + pkt[pos + 1]) & 0x1fff;
			pos += 2;
			if (prog >= pat_use) {
				fprintf(stderr, "logic error: pat_use=%u prog=%u\n", pat_use, prog);
			} else {
				pat[prog] = childpid;
			}
		}
		break;
	}

	case 0x1ffb:	// ATSC PSIP tables like STT, TVCT / CVCT, and MGT
		switch (pkt[pos]) {
		case 0xc7:	// MGT
			if (atsc_parse_hdr(pkt, len, &pos, &mgt_curver, "MGT")) return pos ? 1 : 0;
			if (parse_mgt(pkt, len, pos)) return 1;
			break;

		case 0xc8:	// TVCT (terrestrial)
		{
			u32 prev_curver = vct_curver;
			if (atsc_parse_hdr(pkt, len, &pos, &prev_curver, "TVCT")) {
				vct_curver = prev_curver;
				return pos ? 1 : 0;
			}
			vct_curver = prev_curver;
			if (parse_vct(pkt, len, pos, "TVCT")) return 1;
			break;
		}

		case 0xc9:	// CVCT (cable)
		{
			u32 prev_curver = vct_curver;
			if (atsc_parse_hdr(pkt, len, &pos, &prev_curver, "CVCT")) {
				vct_curver = prev_curver;
				return pos ? 1 : 0;
			}
			vct_curver = prev_curver;
			if (parse_vct(pkt, len, pos, "CVCT")) return 1;
			break;
		}

		case 0xda:	// SVCT (satellite)
		{
			u32 prev_curver = vct_curver;
			if (atsc_parse_hdr(pkt, len, &pos, &prev_curver, "SVCT")) {
				vct_curver = prev_curver;
				return pos ? 1 : 0;
			}
			vct_curver = prev_curver;
			if (parse_vct(pkt, len, pos, "SVCT")) return 1;
			break;
		}

		case 1:	// 0x301 is listed in MGT, but how do you get the high 8 bits to verify that?
		case 9:
		case 0x58:
		case 0x6e:
		case 0x6f:
		case 0xf0:
		case 0xca:	// RRT
		case 0xcb:	// DET Data Event Table
		case 0xcd:	// STT
		case 0xcf:	// DST Data Service Table
		case 0xd1:	// NRT Network Resources Table
		case 0xd3:	// DCCT Directed Channel Change Table
			break;
		default:	// unknown PSIP table
			fprintf(stderr, "ATSC PSIP %x unknown\n", pkt[pos]);
			return 1;
		}
		break;

	case 0x1fff:	// null pid used for padding
		break;

	default:
		if (pid == pmt_pid) {
			// note: pmt_pid is initialized to 0. case 0 catches PID 0 above so this is only reached by the real pmt_pid
			u32 id = 0;
			if (mpg_parse_hdr(pkt, len, &pos, &id, &pmt_curver, "PMT", 2 /*PMT tblid*/)) return pos ? 1 : 0;
			if (id && id != 3) fprintf(stderr, "Warn: PMT with prog=%u (should be 3)\n", id);
			pcr_pid = (((u32) pkt[pos] << 8) + pkt[pos + 1]) & 0x1fff;
			//dump_pkt(pkt + pos, 26);
		}
		break;
	}
	return 0;
}

static const unsigned max_pkt_len = 0x1003;
int mpgatsc::thread_demux(u8 pkt[188])
{
	if (pkt[0] != 0x47) {
		fprintf(stderr, "thread_demux: sync byte is %02x not 47\n", pkt[0]);
		return 1;
	}
	if (pkt[1] & 0x80) {
		//fprintf(stderr, "thread_demux: FEC fail\n");
		return 0;
	}
	if (dumpfile) {
		if (fwrite(pkt, 1, 188, (FILE *) dumpfile) != 188) {
			fprintf(stderr, "thread_demux: write(%s) failed: %d %s\n", dumpfilename, errno, strerror(errno));
			fclose((FILE *) dumpfile);
			dumpfile = 0;
		}
	}

	u32 pid = (((u32) pkt[1] << 8) + pkt[2]) & 0x1fff;
	u32 pos = 4;

	if (pkt[3] & 0x20) {
		if (pid == 0) fprintf(stderr, "thread_demux: PAT with adaptation %02x\n", pkt[4]);
		pos += pkt[pos] + 1;
		if (pos > 188) {
			fprintf(stderr, "Warn: thread_demux: adaptation len %u invalid\n", pos);
			return 0;
		} else if (pos == 188) {
			// purely adaptation data
			return 0;
		}
	}

	if (pkt[1] & 0x40) {	// Payload Unit Start
		if (pktlist[pid] && pktlen[pid]) {
			if ((pid < 8 || pid > 0x65) && (pid < 0x1000 || pid > 0x1e7f) &&
				pid != 0x1408 && pid != 0x1ffa)
			{
				if (pid == 0x1ffb) {
					if (pktlist[pid][0] < 0xc8 || pktlist[pid][0] > 0xca)
						fprintf(stderr, "thread_demux: Warn: pid %x tbl %x never had full len\n", pid, pktlist[pid][0]);
				} else fprintf(stderr, "thread_demux: Warn: pid %x never had full len\n", pid);
			}
			// throw away incomplete pkt
		}

		// start new pkt
		pos += pkt[pos] + 1;	// pointer_field
		if (pos >= 188) {
			fprintf(stderr, "Warn: thread_demux: unit start len %u\n", pos);
			pktlen[pid] = 0;
			return 0;
		}

		if (!pktlist[pid]) {
			pktlist[pid] = (typeof(pktlist[0])) malloc(max_pkt_len);
			if (!pktlist[pid]) {
				fprintf(stderr, "thread_demux: failed to malloc pkt pid %x\n", pid);
				pktlen[pid] = 0;
				return 1;
			}
		}
		pktlen[pid] = 188 - pos;
		memcpy(pktlist[pid], &pkt[pos], 188 - pos);

		// with pktlen[pid] != 0 the pkt now collects bytes until pktlen[pid] >= section_length
	} else if (pktlist[pid]) {
		if (!pktlen[pid]) return 0;	// must find Payload Unit Start to identify start of packet

		// collect bytes
		u32 len = 188 - pos;
		if (pktlen[pid] + len >= max_pkt_len) {
			len = max_pkt_len - pktlen[pid];
			fprintf(stderr, "thread_demux: Warn: pkt too big pid %x\n", pid);
			if (!len) return 0;
		}
		memcpy(pktlist[pid] + pktlen[pid], &pkt[pos], len);
		pktlen[pid] += len;
	}

	if (pktlen[pid]) {
		// see if pktlen[pid] >= section_length
		u32 slen = ((((u32) pktlist[pid][1] << 8) + pktlist[pid][2]) & 0xfff) + 3;

		if (pktlen[pid] >= slen) {
			// yes, full section_length found: can parse pkt now
			if (parse_tbl(pktlist[pid], slen, pid)) {
				fprintf(stderr, "pid=%x len=%x ", pid, slen);
				dump_pkt(pktlist[pid], pktlen[pid]);
			}

			// reset pktlen[pid] so any additional bytes get discarded until next Payload Unit Start
			pktlen[pid] = 0;
		}
	}
	return 0;
}

int mpgatsc::open_dump(const char * filename)
{
	strncpy(dumpfilename, filename, sizeof(dumpfilename) - 1);
	dumpfilename[sizeof(dumpfilename) - 1] = 0;
	dumpfile = fopen(dumpfilename, "wb");
	if (!dumpfile) {
		fprintf(stderr, "mpgatsc::open_dump(%s) ch%02u failed: %d %s\n", filename, tvch, errno, strerror(errno));
		return 1;
	}
	fprintf(stderr, "dumpfile=%p this=%p\n", dumpfile, this);
	return 0;
}
