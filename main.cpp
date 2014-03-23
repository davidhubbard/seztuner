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
#include <unistd.h>
#include <stdlib.h>
#include <termios.h>
#include <sys/time.h>
#include "mpgts.h"

using namespace tuner_ns;

static void scan_progress_cb(void * ctx, unsigned idx, unsigned max)
{
	printf("\r\e[K%s scan... %u/%u", (const char *) ctx, idx, max);
	fflush(stdout);
}

static int rawgetch()
{
	// see http://stackoverflow.com/questions/2984307/c-key-pressed-in-linux-console
	struct termios orig_term_attr;
	struct termios new_term_attr;

	if (tcgetattr(fileno(stdin), &orig_term_attr)) {
		fprintf(stderr, "tcgetattr(stdin) failed: %d %s\n", errno, strerror(errno));
		return -1;
	}

	memcpy(&new_term_attr, &orig_term_attr, sizeof(struct termios));
	new_term_attr.c_lflag &= ~(ECHO|ICANON);
	new_term_attr.c_cc[VTIME] = 0;
	new_term_attr.c_cc[VMIN] = 0;
	if (tcsetattr(fileno(stdin), TCSANOW, &new_term_attr)) {
		fprintf(stderr, "tcsetattr(stdin, raw) failed: %d %s\n", errno, strerror(errno));
		return -1;
	}

	int r = fgetc(stdin);

	// it is important to restore stdin before handling what fgetc returned
	if (tcsetattr(fileno(stdin), TCSANOW, &orig_term_attr)) {
		fprintf(stderr, "tcsetattr(stdin, orig) failed: %d %s\n", errno, strerror(errno));
		return -1;
	}

	if (r == EOF) {
		if (!errno) return 0;	// no key returned by fgetc()
		fprintf(stderr, "fgetc(stdin) failed: %d %s\n", errno, strerror(errno));
		return -1;
	}
	return r;
}

static int get_ch_id(mpgts * itm, unsigned n_ch, unsigned * chlist)
{
	size_t vct_max = 65536;
	char * vct_all = (typeof(vct_all)) malloc(sizeof(*vct_all) * vct_max);
	if (!vct_all) {
		fprintf(stderr, "failed to malloc vct_all\n");
		return 1;
	}
	vct_all[0] = 0;

	printf("freq lock phase_mse eq_mse | freq lock phase_mse eq_mse | cycle\n");
	static const unsigned tuner_max_ch = tuner::NUM_CHANNELS;
	unsigned scan;
	unsigned go = 1;
	for (scan = 0; go && n_ch; ) {
		// tune two channels
		unsigned i = scan;
		u8 ch;
		for (ch = 0; ch < tuner_max_ch; ch++) {
			if (itm->get_freq(ch) != chlist[i]) {
				printf(" %2u   --  ----      ----   | ", chlist[i]);
				fflush(stdout);
				if (itm->set_freq(ch, chlist[i])) {
					free(vct_all);
					return 1;
				}
			}
			i++;
			if (i >= n_ch) break;
		}

		u8 status[tuner_max_ch];
		for (ch = 0; ch < tuner_max_ch; ch++) status[ch] = 0;

		// wait for tuner to lock
		#define tune_nl "\r"
		unsigned j;
		for (j = 0; j < 100; j++) {
			// if all channels are done
			for (ch = 0; ch < tuner_max_ch; ch++) if (!(status[ch] & 0x40)) break;
			if (ch >= tuner_max_ch) {
				j = 10000000;	// stop after this loop
			} else {
				if (j) {
					printf("%u (any key to abort)", 100 - j);
					fflush(stdout);
					int r = rawgetch();
					if (r == -1) {
						free(vct_all);
						return 1;
					}
					if (r) {
						go = 0;
						break;
					}
				}
				usleep(160000);
			}
			i = scan;
			for (ch = 0; ch < tuner_max_ch; ch++) {
				if ((status[ch] & 0xf) != 0xf) {
					u32 ptmse, eqmse;
					if (itm->get_mse(ch, &status[ch], &ptmse, &eqmse)) {
						free(vct_all);
						return 1;
					}
					printf("%s %2u   %2x  %4x      %4x   | ", (ch == 0) ? tune_nl "\e[K" : "",
						chlist[i], status[ch], ptmse >> 4, eqmse >> 4);
				} else if (status[ch] & 0x40) {
					u8 ignore;
					u32 ptmse, eqmse;
					if (itm->get_mse(ch, &ignore, &ptmse, &eqmse)) {
						free(vct_all);
						return 1;
					}
					printf("%s %2u done%u %4x      %4x   | ", (ch == 0) ? tune_nl "\e[K" : "",
						chlist[i], ch, ptmse >> 4, eqmse >> 4);
				} else {
					printf("%s %2u start %u                | ", (ch == 0) ? tune_nl "\e[K" : "", chlist[i], ch);
					if ((status[ch] & 0x20) == 0) {
						if (itm->start_ts(ch)) {
							free(vct_all);
							return 1;
						}
						status[ch] |= 0x20;
					}
					if (itm->get_vct(ch)) {
						// stop this TS, wait for all channels before displaying VCT data
						if (itm->stop_ts(ch)) {
							free(vct_all);
							return 1;
						}
						status[ch] |= 0x40;
					}
				}

				i++;
				if (i >= n_ch) break;
			}
		}

		// get VCT data for any channel that succeeded, and then remove that channel
		i = scan;
		for (ch = 0; ch < tuner_max_ch; ch++, i++) if (status[ch] & 0x40) {
			if (i >= n_ch) break;
			size_t len = strlen(vct_all) + strlen(itm->get_vct(ch)) + 1;
			if (len > vct_max) {
				vct_all = (typeof(vct_all)) realloc(vct_all, sizeof(*vct_all) * (vct_max *= 2));
				if (!vct_all) {
					fprintf(stderr, "failed to realloc vct_all to %zu\n", vct_max);
					return 1;
				}
			}
			strcat(vct_all, itm->get_vct(ch));
			// remove from chlist because it has been successfully tuned
			n_ch--;
			memmove(&chlist[i], &chlist[i + 1], (n_ch - i)*sizeof(chlist[0]));
			i--;
		}
		if (!n_ch) break;
		printf(tune_nl);

		// TODO: fix low VHF channels (2-6)
	}

	if (vct_all[0]) {
		printf("\nfreq digital channel: (channel name can be found on wikipedia)\n%s", vct_all);
	}
	free(vct_all);
	return 0;
}


static int do_item(unsigned idx, mpgts * itm, tuner::tuner_antennas selected_antenna)
{
	char dstr[256]; ip_printf(dstr, itm->get_ip());
	(void) idx;

	if (itm->open()) {
		fprintf(stderr, "%s failed\n", dstr);
		return 1;
	}

	unsigned n_ch = 0, * chlist = 0;
	if (selected_antenna != tuner::nc) {
		if (itm->set_antenna(selected_antenna)) {
			fprintf(stderr, "%s failed to select antenna\n", dstr);
			return 1;
		}
	}

	if (itm->scan(&n_ch, &chlist)) {
		fprintf(stderr, "%s failed to test antenna\n", dstr);
		return 1;
	}
	if (selected_antenna == tuner::nc)
		printf("%s auto-detected -a%u\n", dstr, (unsigned) itm->get_antenna());

	{
		unsigned n2 = 0, * chlist2 = 0;
		if (itm->scan(&n2, &chlist2, scan_progress_cb, dstr, 80)) return 1;

		// trim "169.254" from front of dstr
		if (!strncmp(dstr, "169.254", 7)) memmove(dstr, &dstr[7], strlen(dstr) - 6);

		printf("\n%s all carrier freqs:", dstr);	// the extra \n at the beginning is due to scan_progress_cb
		unsigned i;
		for (i = 0; i < n2; i++) printf(" %u", chlist2[i]);
		printf("\n");
		free(chlist2);
		//
		// the strength of a channel directly leads to a faster carrier detect
		// that is why scan(cr_ms = 80) picks up a lot more carrier frequencies than the default 20
		// throw away chlist2
		//
	}

	//
	// use chlist from the default 20, then sample the output of the full tuner, not just carrier detect
	// this seems slow, the following optimizations are in play:
	//    chlist from default 20 has only the strongest channels
	//    use both tuners in parallel
	//
	printf("%s strong freqs:", dstr);
	fflush(stdout);

	struct timeval t_start;
	gettimeofday(&t_start, 0);
	unsigned long end[tuner::NUM_CHANNELS];
	unsigned    endch[tuner::NUM_CHANNELS];
	memset(end, 0, sizeof(end));
	memset(endch, 0, sizeof(endch));

	unsigned strongch[n_ch];
	unsigned strongch_use = 0;

	unsigned dbg = 0;
	unsigned i;
	for (i = 0; i < n_ch; i++) {
		// find an available tuner channel
		//
		// be aware of the confusing use of "channel" to refer to a TV frequency (MHz)
		// and the dual-tuner design of the sezmi tuner (2 channels can be tuned simultaneously)
		//
		// this variable 'ch' is for selecting one of the 2 tuners
		u8 ch = 0;
		unsigned long cur_ms;
		for (;;) {
			struct timeval t_now;
			gettimeofday(&t_now, 0);
			cur_ms = (t_now.tv_sec - t_start.tv_sec)*1000 + ((int) t_now.tv_usec - (int) t_start.tv_usec)/1000;

			if (!end[ch]) break;	// end[ch] == 0 means this ch can be used

			// finish running tune on end[ch]
			if (end[ch] < cur_ms) {
				// tune timed out, channel is weak
				if (dbg) {
					printf(" %u.%u:n", endch[ch], ch);
					fflush(stdout);
				}
				end[ch] = 0;
				break;
			} else {
				u8 status;
				u32 ptmse, eqmse;
				if (itm->get_mse(ch, &status, &ptmse, &eqmse)) return 1;
				//printf(" %2x p %5x e %5x", status, ptmse, eqmse);
				if (status > 3) {
					if (dbg) printf(" %u.%u:y", endch[ch], ch);
						else printf(" %u", endch[ch]);
					fflush(stdout);
					strongch[strongch_use++] = endch[ch];
					end[ch] = 0;
					break;
				}
			}

			ch++;
			if (ch >= tuner::NUM_CHANNELS) {
				usleep(2*50000);
				ch = 0;
			}
		}

		end[ch] = cur_ms + 1000;
		endch[ch] = chlist[i];
		if (itm->set_freq(ch, endch[ch])) return 1;
		if (dbg) printf(" start%u.%u", endch[ch], ch);
		fflush(stdout);
		usleep(2*50000);
	}

	// finish running tunes
	for (;;) {
		unsigned count = 0;
		unsigned long cur_ms;
		for (u8 ch = 0; ch < tuner::NUM_CHANNELS; ch++) {
			struct timeval t_now;
			gettimeofday(&t_now, 0);
			cur_ms = (t_now.tv_sec - t_start.tv_sec)*1000 + ((int) t_now.tv_usec - (int) t_start.tv_usec)/1000;

			if (!end[ch]) continue;	// end[ch] == 0 means this ch is done

			// finish running tune on end[ch]
			if (end[ch] < cur_ms) {
				// tune timed out, channel is weak
				if (dbg) {
					printf(" %u.%u:n", endch[ch], ch);
					fflush(stdout);
				}
				end[ch] = 0;
			} else {
				u8 status;
				u32 ptmse, eqmse;
				if (itm->get_mse(ch, &status, &ptmse, &eqmse)) return 1;
				//printf(" %2x p %5x e %5x", status, ptmse, eqmse);
				if (status > 3) {
					if (dbg) printf(" %u.%u:y", endch[ch], ch);
						else printf(" %u", endch[ch]);
					strongch[strongch_use++] = endch[ch];
					fflush(stdout);
					end[ch] = 0;
				} else {
					count++;
				}
			}
		}
		if (!count) break;
		usleep(2*50000);
	}
	printf("\n");
	free(chlist);

	i = (unsigned) get_ch_id(itm, strongch_use, strongch);
	itm->close();
	return (int) i;
}

int main(int argc, char ** argv)
{
	tuner::tuner_antennas selected_antenna = tuner::nc;
	unsigned i;
	for (i = 1; (int) i < argc; i++) {
		if (!strncmp(argv[i], "-a", 2) && strlen(argv[i]) == 3 &&
			argv[i][2] >= '1' && argv[i][2] <= '3')
		{
			switch (argv[i][2]) {
			case '1': selected_antenna = tuner::ant1; break;
			case '2': selected_antenna = tuner::ant2; break;
			case '3': selected_antenna = tuner::coax; break;
			}
		} else {
			fprintf(stderr, 
				"Usage: %s [ -a1 | -a2 | -a3 ]   +---------------------------------+\n"
				"    -a1 = use Sezmi Antenna 1   | Antenna 2    Coax    Antenna 1  |\n"
				"    -a2 = use Sezmi Antenna 2   | Power    Ethernet to Sezmi ...  |\n"
				"    -a3 = use Coax Antenna      +---------------------------------+\n"
				"    This is just an example of how to use the tuner.\n"
				"    It dumps the TVCT channel names of any ATSC channel it can find.\n",
				argv[0]);
			return 1;
		}
	}

	unsigned list_use = 0;
	mpgts * list = mpgts::find(&list_use);
	if (!list) return 1;
	if (!list_use) {
		fprintf(stderr, "Error: no tuners found\n");
		return 1;
	}
	printf("%s found %u IP%s, probing in order found:\n", argv[0], list_use, list_use == 1 ? "" : "s");

	for (i = 0; i < list_use; i++) {
		if (do_item(i, &list[i], selected_antenna)) {
			free(list);
			return 1;
		}
	}

	free(list);
	return 0;
}
