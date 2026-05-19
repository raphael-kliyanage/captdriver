/*
 * Copyright (C) 2013 Alexey Galakhov <agalakhov@gmail.com>
 * Copyright (C) 2016 Alexei Gordeev <KP1533TM2@gmail.com>
 *
 * Licensed under the GNU General Public License Version 3
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "std.h"
#include "word.h"
#include "capt-command.h"
#include "capt-status.h"
#include "generic-ops.h"
#include "hiscoa-common.h"
#include "hiscoa-compress.h"
#include "paper.h"
#include "printer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint16_t job;

struct printer_gpio_s {
	const uint8_t (*init);
	const uint8_t (*blink);
};

struct lbp2900_ops_s {
	struct printer_ops_s ops;
	struct printer_gpio_s gpio;

	const struct capt_status_s * (*get_status) (void);
	void (*wait_ready) (void);
};

static const uint8_t magicbuf_0[] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const uint8_t magicbuf_2[] = {
	0xEE, 0xDB, 0xEA, 0xAD, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const uint8_t magicbuf_linux_online[] = {
	0xEE, 0xDB, 0xEA, 0xAD, 0x00, 0x00, 0x00, 0x00
};

static const uint8_t lbp2900_gpio_blink[] = {
	0x00, 0x00, 0x01, 0x02, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x01, 0x00,
};

static const uint8_t lbp2900_gpio_init[] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
};

static const uint8_t lbp3000_job_init[] = {
	0x00, 0x00,
};

static const uint8_t lbp3010_gpio_blink[] = {
        /* led */ 0x31, 0x00, 0x00, /* S6 */ 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, /* S7 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t lbp3010_gpio_init[] = {
        /* led */ 0x13, 0x00, 0x00, /* S6 */ 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, /* S7 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t lbp6000_job_init[] = {
        0x01, 0x00,
};

static const struct capt_status_s *lbp2900_get_status(const struct printer_ops_s *ops)
{
	const struct lbp2900_ops_s *lops = container_of(ops, struct lbp2900_ops_s, ops);
	return lops->get_status();
}

static void lbp2900_wait_ready(const struct printer_ops_s *ops)
{
	const struct lbp2900_ops_s *lops = container_of(ops, struct lbp2900_ops_s, ops);
	lops->wait_ready();
}

static void capt_get_printer_info(struct printer_state_s *state)
{
	uint8_t buf[32];
	size_t size = sizeof(buf);
	capt_sendrecv(CAPT_GET_PRINTER_INFO, NULL, 0, buf, &size);
	if (size >= 10) {
		state->printer_blk = WORD(buf[6], buf[7]);
		state->printer_buf = WORD(buf[8], buf[9]);
		fprintf(stderr, "DEBUG: CAPT: GetPrinterInfo Blk=0x%04x (%u) Buf=0x%04x (%u)\n",
			state->printer_blk, state->printer_blk,
			state->printer_buf, state->printer_buf);
	} else {
		/* Default values for LBP3000 if reply is too short */
		state->printer_blk = 65520;
		state->printer_buf = 64;
		fprintf(stderr, "DEBUG: CAPT: GetPrinterInfo reply too short (%u), using defaults\n",
			(unsigned)size);
	}
}

static void send_job_start(uint8_t fg)
{
	/* host/user/document name lengths: all zero (no strings appended) */
	uint8_t ml = 0x00;
	uint8_t ul = 0x00;
	uint8_t nl = 0x00;

	time_t rawtime = time(NULL);
	const struct tm *tm = localtime(&rawtime);

	/* TZOffset: protocol uses seconds-west-of-UTC / 60.
	 * `timezone` (from <time.h>) is seconds WEST of UTC (set by localtime()).
	 * For Moscow UTC+3: timezone = -10800 → TZOffset = -180 = 0xFF4C LE.
	 * Both TZOffset (bytes 20-21) and TZOffset2 (bytes 22-23) must be equal. */
	int16_t tz = (int16_t)(timezone / 60);

	/* Year must be 1900 + tm_year (e.g. 2026).
	 * Month must be 1-indexed (1=Jan … 12=Dec). */
	uint16_t year = (uint16_t)(tm->tm_year + 1900);
	uint8_t month = (uint8_t)(tm->tm_mon + 1);

	/* CAPT_SET_JOB_INFO2 payload format:
	 * bytes  0- 7: hard-coded zeros (bytes 1-8 in 1-indexed protocol)
	 * bytes  8- 9: HostLen (uint16 LE)
	 * bytes 10-11: UserLen (uint16 LE)
	 * bytes 12-13: JobNameLen (uint16 LE)
	 * bytes 14-15: hard-coded zeros
	 * byte  16: JobFlag
	 * byte  17: NumberUp = 1
	 * bytes 18-19: JobID (uint16 LE)
	 * bytes 20-21: TZOffset (int16 LE)
	 * bytes 22-23: TZOffset2 (same value, int16 LE)
	 * bytes 24-25: Year (uint16 LE)
	 * byte  26: Month (1-indexed)
	 * byte  27: Day
	 * byte  28: Hour
	 * byte  29: Minute
	 * byte  30: Second
	 * byte  31: hard-coded zero (NOT 0x01)
	 * bytes 32-71: hard-coded zeros
	 * Total payload: 72 bytes */
	uint8_t buf[72];
	memset(buf, 0, sizeof(buf));
	buf[8]  = ml;
	buf[10] = ul;
	buf[12] = nl;
	buf[16] = fg;
	buf[17] = 0x01; /* NumberUp */
	buf[18] = LO(job);
	buf[19] = HI(job);
	buf[20] = (uint8_t)(tz & 0xFF);
	buf[21] = (uint8_t)((tz >> 8) & 0xFF);
	buf[22] = (uint8_t)(tz & 0xFF);
	buf[23] = (uint8_t)((tz >> 8) & 0xFF);
	buf[24] = LO(year);
	buf[25] = HI(year);
	buf[26] = month;
	buf[27] = (uint8_t)tm->tm_mday;
	buf[28] = (uint8_t)tm->tm_hour;
	buf[29] = (uint8_t)tm->tm_min;
	buf[30] = (uint8_t)tm->tm_sec;
	/* buf[31] = 0x00 (already zeroed) */

	capt_sendrecv(CAPT_SET_JOB_INFO2, buf, sizeof(buf), NULL, 0);
}

/*
 * ReserveUnit with error recovery (all job prologues).
 *
 * ReserveUnit reply byte 0: if bit 0x80 is set the printer is in an error
 * state (e.g. a stale job from a previous run).  Perform the canonical clear
 * sequence (GoOffline → DiscardData → ClearMisPrint → ClearError → GoOnline)
 * and retry once.  Returns the job ID extracted from buf[2:3].
 */
static uint16_t capt_reserve_unit(struct printer_state_s *state)
{
	uint8_t buf[8];
	size_t size = sizeof(buf);

	capt_sendrecv(CAPT_RESERVE_UNIT, magicbuf_0, ARRAY_SIZE(magicbuf_0), buf, &size);

	if (size >= 1 && (buf[0] & 0x80)) {
		fprintf(stderr, "DEBUG: CAPT: ReserveUnit error flag 0x%02x — clearing and retrying\n",
			buf[0]);

		/* Go offline first if not already offline */
		const struct capt_status_s *status = lbp2900_get_status(state->ops);
		if (!FLAG(status, CAPT_FL_OFFLINE)) {
			capt_sendrecv(CAPT_GO_OFFLINE, lbp3000_job_init, ARRAY_SIZE(lbp3000_job_init), NULL, 0);
			for (int i = 0; i < 50; i++) {
				status = lbp2900_get_status(state->ops);
				if (FLAG(status, CAPT_FL_OFFLINE))
					break;
				usleep(100000);
			}
		}
		capt_sendrecv(CAPT_DISCARD_DATA, NULL, 0, NULL, 0);
		capt_sendrecv(CAPT_CLEAR_MIS_PRINT, NULL, 0, NULL, 0);
		capt_sendrecv(CAPT_CLEAR_ERROR, NULL, 0, NULL, 0);
		capt_sendrecv(CAPT_GO_ONLINE, magicbuf_linux_online, ARRAY_SIZE(magicbuf_linux_online), NULL, 0);
		lbp2900_wait_ready(state->ops);

		/* Retry */
		size = sizeof(buf);
		capt_sendrecv(CAPT_RESERVE_UNIT, magicbuf_0, ARRAY_SIZE(magicbuf_0), buf, &size);
		if (size >= 1 && (buf[0] & 0x80)) {
			fprintf(stderr, "ERROR: CAPT: ReserveUnit failed (0x%02x) after error recovery — aborting\n",
				buf[0]);
			exit(1);
		}
	}

	return (size >= 4) ? WORD(buf[2], buf[3]) : 0;
}

static void lbp2900_job_prologue(struct printer_state_s *state)
{
	capt_get_printer_info(state);
	//sleep(1);
	capt_init_status();
	lbp2900_get_status(state->ops);

	capt_sendrecv(CAPT_START_0, NULL, 0, NULL, 0);
	job = capt_reserve_unit(state);

	capt_sendrecv(CAPT_SET_LED_STATUS, lbp3010_gpio_init, ARRAY_SIZE(lbp3010_gpio_init), NULL, 0);
	lbp2900_wait_ready(state->ops);

	send_job_start(1);
	lbp2900_wait_ready(state->ops);
}

static void lbp3000_job_prologue(struct printer_state_s *state)
{
	const struct capt_status_s *status;

	state->sent_job_cont = false;
	state->printer_page_offset = 0;

	capt_get_printer_info(state);
	//sleep(1);
	capt_init_status();
	lbp2900_get_status(state->ops);

	for (int i = 0; i < 50; i++) {
		status = lbp2900_get_status(state->ops);
		if (!FLAG(status, CAPT_FL_NEED_INPUT_STATUS))
			break;
		capt_sendrecv(CAPT_GET_INPUT_STATUS, NULL, 0, NULL, 0);
		usleep(200000);
	}

	job = capt_reserve_unit(state);

	send_job_start(CAPT_JOBFLAG_START);

	status = lbp2900_get_status(state->ops);
	if (!(FLAG(status, CAPT_FL_OFFLINE))) {
		capt_sendrecv(CAPT_GO_OFFLINE, lbp3000_job_init, ARRAY_SIZE(lbp3000_job_init), NULL, 0);
		for (int i = 0; i < 50; i++) {
			status = lbp2900_get_status(state->ops);
			if (FLAG(status, CAPT_FL_OFFLINE))
				break;
			usleep(100000);
		}
		capt_get_xstatus_only();
	}

	capt_sendrecv(CAPT_DISCARD_DATA, NULL, 0, NULL, 0);
	capt_sendrecv(CAPT_CLEAR_MIS_PRINT, NULL, 0, NULL, 0);
	capt_sendrecv(CAPT_CLEAR_ERROR, NULL, 0, NULL, 0);
	capt_sendrecv(CAPT_GO_ONLINE, magicbuf_linux_online, ARRAY_SIZE(magicbuf_linux_online), NULL, 0);

	lbp2900_wait_ready(state->ops);
	capt_get_xstatus_only();
	lbp2900_get_status(state->ops);
}

static void lbp3010_job_prologue(struct printer_state_s *state)
{
	capt_get_printer_info(state);
	//sleep(1);
	capt_init_status();
	lbp2900_get_status(state->ops);

	capt_sendrecv(CAPT_START_0, NULL, 0, NULL, 0);
	job = capt_reserve_unit(state);

	capt_sendrecv(CAPT_SET_LED_STATUS, lbp3010_gpio_init, ARRAY_SIZE(lbp3010_gpio_init), NULL, 0);
	lbp2900_wait_ready(state->ops);

	send_job_start(1);
	lbp2900_wait_ready(state->ops);
}

static void lbp6000_job_prologue(struct printer_state_s *state)
{
	capt_get_printer_info(state);
	sleep(1);
	capt_init_status();
	lbp2900_get_status(state->ops);

	capt_sendrecv(CAPT_START_0, NULL, 0, NULL, 0);
	job = capt_reserve_unit(state);

	capt_sendrecv(CAPT_SET_LED_STATUS, lbp3010_gpio_init, ARRAY_SIZE(lbp3010_gpio_init), NULL, 0);
	lbp2900_wait_ready(state->ops);

	capt_sendrecv(CAPT_LBP6000_SETUP_0, lbp6000_job_init, ARRAY_SIZE(lbp6000_job_init), NULL, 0);
	lbp2900_wait_ready(state->ops);

	send_job_start(1);
	lbp2900_wait_ready(state->ops);
}

static bool lbp2900_page_prologue(struct printer_state_s *state, const struct page_dims_s *dims)
{
	const struct capt_status_s *status;
	size_t s;
	uint8_t buf[16];

	uint8_t sz = 0x00; /* paper size byte from tPaperSizeTbl */
	uint8_t save = dims->toner_save; /* TonerSave */
	uint8_t fm = 0x00; /* MediaType (fuser/media mode) */

	uint8_t td = (dims->ink_k > 0) ? (uint8_t)(dims->ink_k << 2) : 0x1f;
	uint8_t paper_type = 0x00;

	switch (dims->media_type) {
		case 0x00:
		case 0x02:
			/* Plain Paper & Plain Paper L */
			fm = 0x01;
			paper_type = 0x00;
			break;
		case 0x01:
			/* Thick Paper */
			fm = 0x01;
			paper_type = 0x00;
			break;
		case 0x03:
			/* Thick Paper H */
			fm = 0x02;
			paper_type = 0x00;
			break;
		case 0x04:
			/* Transparency */
			fm = 0x13;
			paper_type = 0x24;
			break;
		case 0x05:
			/* Transparency */
			fm = 0x14;
			paper_type = 0x24;
			break;
		case 0x06:
			/* Envelope */
			fm = 0x1C;
			paper_type = 0x20;
			break;
		default:
			fm = 0x01;
			paper_type = 0x00;
	}
	fprintf(stderr, "DEBUG: CAPT: media_type=%u, fm=%u, paper_type=0x%02x\n",
		dims->media_type, fm, paper_type);

	if ( strncmp(dims->media_size, "A4", 2) == 0 ) sz = 0x02;
	else if ( strncmp(dims->media_size, "A5", 2) == 0 ) sz = 0x03;
	else if ( strncmp(dims->media_size, "B5", 2) == 0 ) sz = 0x07;
	else if ( strncmp(dims->media_size, "Executive", 9) == 0 ) sz = 0x0A;
	else if ( strncmp(dims->media_size, "Legal", 5) == 0 ) sz = 0x0C;
	else if ( strncmp(dims->media_size, "Letter", 6) ==0 ) sz = 0x0D;
	else if ( strncmp(dims->media_size, "EnvC5", 5) ==0 ) sz = 0x15;
	else if ( strncmp(dims->media_size, "Env10", 5) == 0 ) sz = 0x16;
	else if ( strncmp(dims->media_size, "EnvMonarch", 10) == 0 ) sz = 0x17;
	else if ( strncmp(dims->media_size, "EnvDL", 5) == 0 ) sz = 0x18;
	else if ( strncmp(dims->media_size, "3x5", 3) ==0 ) sz = 0x40;
	else if ( strncmp(dims->media_size, "PRC16K", 6) ==0 ) sz = 0xD4;
	else sz = 0x02;
	fprintf(stderr, "DEBUG: CAPT: media_size=%s, sz=0x%02x\n", dims->media_size, sz);

	/* Save paper size byte for use in out-of-paper SetLEDStatus payload. */
	state->paper_sz = sz;

	/* Reset streaming-mode flag for each new page. */
	state->startprint_sent = false;

	/* IC_BEGIN_PAGE (D0A0) payload format:
	 * idx  0- 1: PageSeq (set to 0x0000 here; sequence filled per page)
	 * idx  2- 3: ModelConst = 0x2a30 for LBP3000 (LE: 0x30, 0x2a)
	 * idx  4   : PaperSzByte
	 * idx  5   : Unk6 = 0x00 (Windows value)
	 * idx  6   : PaperSrc = 0x00 (auto-feed)
	 * idx  7   : 0x00
	 * idx  8-11: TonerDensity — all 4 bytes equal (default 0x1f)
	 * idx 12   : PaperType (special_mode_for_papertype: 0x00=Plain, 0x20=Env, 0x24=Trans)
	 * idx 13   : ResFlag = 0x11 (600 dpi constant for LBP3000, NOT media_adapt)
	 * idx 14   : Fixed_04 = 0x04 (CNTblModel=1)
	 * idx 15   : Fixed_00 = 0x00
	 * idx 16   : Fixed_01 = 0x01
	 * idx 17   : Fixed_01b = 0x01
	 * idx 18   : SuperSmooth = 0x02 (CNSuperSmooth for LBP3000)
	 * idx 19   : TonerSave = 0x00 off by default
	 * idx 20   : Unk21 = 0x00 (Windows value)
	 * idx 21   : 0x00 for LBP3000
	 * idx 22-23: MarginH uint16 LE
	 * idx 24-25: MarginW uint16 LE
	 * idx 26-27: LineSize uint16 LE
	 * idx 28-29: ImgHeight uint16 LE
	 * idx 30-31: PaperW uint16 LE
	 * idx 32-33: PaperH uint16 LE
	 * idx 34-35: FixFlags uint16 LE = 0x0000 for plain paper, no duplex
	 * idx 36   : MediaType = 0x01 for Plain/Plain L
	 * idx 37-39: 0x00 padding
	 * Total: 40 bytes for LBP3000 (CNTblModel=1)
	 */
	uint8_t pageparms[] = {
		/* idx  0- 7 */
		0x00, 0x00, 0x30, 0x2A, sz, 0x00, 0x00, 0x00,
		/* idx  8-15: TonerDensity, PaperType, ResFlag=0x11, Fixed_04, Fixed_00 */
		td, 0x1C, 0x1C, 0x1C, paper_type, 0x11, 0x04, 0x00,
		/* idx 16-23: Fixed_01, Fixed_01b, SuperSmooth=0x02, TonerSave, Unk21=0x00, 0x00, MarginH LE */
		0x01, 0x01, 0x02, save, 0x00, 0x00,
		LO(dims->margin_height), HI(dims->margin_height),
		/* idx 24-31: MarginW LE, LineSize LE, ImgHeight LE */
		LO(dims->margin_width), HI(dims->margin_width),
		LO(dims->line_size), HI(dims->line_size),
		LO(dims->num_lines), HI(dims->num_lines),
		/* idx 32-39: PaperW LE, PaperH LE, FixFlags=0x0000, MediaType, padding */
		LO(dims->paper_width), HI(dims->paper_width),
		LO(dims->paper_height), HI(dims->paper_height),
		0x00, 0x00, fm, 0x00, 0x00, 0x00,
	};

	status = lbp2900_get_status(state->ops);
	if (FLAG(status, CAPT_FL_UNINIT1) || FLAG(status, CAPT_FL_OFFLINE)) {
		capt_sendrecv(CAPT_CLEAR_MIS_PRINT, NULL, 0, NULL, 0);
		capt_sendrecv(CAPT_CLEAR_ERROR, NULL, 0, NULL, 0);
		capt_sendrecv(CAPT_DISCARD_DATA, NULL, 0, NULL, 0);
		lbp2900_wait_ready(state->ops);

		capt_sendrecv(CAPT_GO_ONLINE, magicbuf_2, ARRAY_SIZE(magicbuf_2), NULL, 0);
		lbp2900_wait_ready(state->ops);
	}

	while (1) {
		if (! FLAG(lbp2900_get_status(state->ops), CAPT_FL_CMD_BUSY))
			break;
		usleep(100000);
	}

	capt_multi_begin(CAPT_MULTI_COMMAND);
	capt_multi_add(CAPT_IC_BEGIN_PAGE, pageparms, sizeof(pageparms));
	s = hiscoa_format_params(buf, sizeof(buf), &hiscoa_default_params);
	capt_multi_add(CAPT_IC_BLACK_PLANE, buf, s);
	capt_multi_add(CAPT_IC_BEGIN_DATA, NULL, 0);
	capt_multi_add(CAPT_IC_END_PAGE, NULL, 0);
	capt_multi_send();

	return true;
}

static void lbp3000_oop_recovery(struct printer_state_s *state)
{
	const struct capt_status_s *status;

	fprintf(stderr, "DEBUG: CAPT: out-of-paper recovery starting\n");

	capt_sendrecv(CAPT_GET_INPUT_STATUS, NULL, 0, NULL, 0);
	lbp2900_get_status(state->ops);
	capt_get_xstatus_only();
	capt_sendrecv(CAPT_CLEAR_MIS_PRINT, NULL, 0, NULL, 0);
	capt_sendrecv(CAPT_CLEAR_ERROR, NULL, 0, NULL, 0);
	capt_sendrecv(CAPT_DISCARD_DATA, NULL, 0, NULL, 0);
	lbp2900_get_status(state->ops);
	capt_get_xstatus_only();
	capt_sendrecv(CAPT_GO_ONLINE, magicbuf_linux_online, ARRAY_SIZE(magicbuf_linux_online), NULL, 0);
	lbp2900_get_status(state->ops);
	capt_get_xstatus_only();
	lbp2900_get_status(state->ops);

	uint8_t led_nopaper[12] = {
		CAPT_INTSTAT_NO_PAPER,  /* byte index 0: int_status */
		0x00,                   /* byte index 1 */
		CAPT_HSCODE_NO_PAPER,   /* byte index 2: hs_code */
		state->paper_sz,        /* byte index 3: paper_size_id */
		0x01,                   /* byte index 4 */
		0x00, 0x00, 0x00,       /* byte index 5-7 */
		/* bytes 9-12 = CAPT_HOSTERR_NO_PAPER = 0x00010000 in LE */
		0x00, 0x00, 0x01, 0x00, /* byte index 8-11 */
	};
	capt_sendrecv(CAPT_SET_LED_STATUS, led_nopaper, sizeof(led_nopaper), NULL, 0);

	lbp2900_get_status(state->ops);
	capt_get_xstatus_only();
	capt_sendrecv(CAPT_GO_OFFLINE, lbp3000_job_init, ARRAY_SIZE(lbp3000_job_init), NULL, 0);
	lbp2900_get_status(state->ops);

	fprintf(stderr, "DEBUG: CAPT: out-of-paper: waiting for user to insert paper\n");

	while (1) {
		status = lbp2900_get_status(state->ops);
		/* CAPT_FL2_NEED_INPUT_STATUS (byte 2 bit 0x02) signals button press */
		if (FLAG(status, CAPT_FL_NEED_INPUT_STATUS))
			break;
		usleep(100000);
	}

	capt_sendrecv(CAPT_GET_INPUT_STATUS, NULL, 0, NULL, 0);
	uint8_t led_clear[12] = { 0 };
	capt_sendrecv(CAPT_SET_LED_STATUS, led_clear, sizeof(led_clear), NULL, 0);
	capt_sendrecv(CAPT_CLEAR_MIS_PRINT, NULL, 0, NULL, 0);
	capt_sendrecv(CAPT_CLEAR_ERROR, NULL, 0, NULL, 0);
	capt_sendrecv(CAPT_DISCARD_DATA, NULL, 0, NULL, 0);
	lbp2900_get_status(state->ops);
	capt_get_xstatus_only();
	capt_sendrecv(CAPT_GO_ONLINE, magicbuf_linux_online, ARRAY_SIZE(magicbuf_linux_online), NULL, 0);
	lbp2900_get_status(state->ops);
	capt_get_xstatus_only();

	fprintf(stderr, "DEBUG: CAPT: out-of-paper recovery complete, counters reset to 0\n");
	/* After recovery, page counters are reset to 0.
	 * Caller must track pages_printed_before_error to know where to resume. */
}

static bool lbp2900_page_epilogue(struct printer_state_s *state, const struct page_dims_s *dims)
{
	(void) dims;
	const struct capt_status_s *status;

	unsigned pipage = state->ipage - state->printer_page_offset;

	capt_send(CAPT_IC_BLACK_END, NULL, 0);

	if (!state->startprint_sent) {
		status = lbp2900_get_status(state->ops);
		if (status->page_decoding != pipage) {
			do {
				usleep(100000);
				status = lbp2900_get_status(state->ops);
			} while (status->page_decoding != pipage);
		}

		uint8_t buf[2] = { LO(pipage), HI(pipage) };
		capt_sendrecv(CAPT_START_PRINT, buf, 2, NULL, 0);
	}

	while (1) {
		status = lbp2900_get_status(state->ops);

		if (!state->sent_job_cont && status->page_completed >= 1) {
			send_job_start(CAPT_JOBFLAG_CONT);
			state->sent_job_cont = true;
		}

		if (status->page_printing >= pipage)
			return true;

		if ((FLAG(status, CAPT_FL_NOTREADY) || FLAG(status, CAPT_FL_OFFLINE))
		    && !FLAG(status, CAPT_FL_PRINTING)
		    && !FLAG(status, CAPT_FL_PROCESSING1)) {
			/* Confirm with GetExtendedStatus */
			capt_get_xstatus_only();
			status = lbp2900_get_status(state->ops);
			if ((status->xstat_cnt & CAPT_CNT_PRINT_REJECTED)
			    && status->xstat_pap == 0x00) {
				fprintf(stderr, "DEBUG: CAPT: out-of-paper confirmed (Cnt=0x%02x, Pap=0x%02x)\n",
					status->xstat_cnt, status->xstat_pap);
				state->pages_printed_before_error = (int)status->page_completed;
				lbp3000_oop_recovery(state);
				state->printer_page_offset = state->ipage - 1;
				fprintf(stderr, "DEBUG: CAPT: printer_page_offset set to %u (ipage=%u)\n",
					state->printer_page_offset, state->ipage);
				return false;
			}
		}

		/*
		 * Fatal hardware errors: paper jam (Eng bit 0x0100) or door open
		 * (Eng bits 0x4000).  These conditions cannot be recovered by the
		 * driver.  Release the reserved unit so the printer is not left
		 * in a locked state, then abort.
		 */
		if (status->xstat_eng & (CAPT_ENG_JAM | CAPT_ENG_DOOR_OPEN)) {
			const char *reason = (status->xstat_eng & CAPT_ENG_JAM)
				? "paper jam" : "door open";
			fprintf(stderr, "ERROR: CAPT: fatal printer error: %s (Eng=0x%04x) — aborting\n",
				reason, status->xstat_eng);
			uint8_t jbuf[2] = { LO(job), HI(job) };
			capt_sendrecv(CAPT_RELEASE_UNIT, jbuf, 2, NULL, 0);
			exit(1);
		}

		/* Legacy no-paper flags from extended status */
		if (FLAG(status, CAPT_FL_NOPAPER2) || FLAG(status, CAPT_FL_NOTREADY)) {
			fprintf(stderr, "DEBUG: CAPT: no paper\n");
			if (FLAG(status, CAPT_FL_PRINTING) || FLAG(status, CAPT_FL_PROCESSING1))
				continue;
			return false;
		}
		usleep(100000);
	}
}

static void lbp2900_job_epilogue(struct printer_state_s *state)
{
	uint8_t jbuf[2] = { LO(job), HI(job) };
	unsigned total_pages = state->ipage;

	capt_get_xstatus_only();
	send_job_start(CAPT_JOBFLAG_END);
	capt_sendrecv(CAPT_RELEASE_UNIT, jbuf, 2, NULL, 0);
	{
		const struct capt_status_s *s;
		while (1) {
			capt_sendrecv(CAPT_GET_INPUT_STATUS, NULL, 0, NULL, 0);
			s = capt_get_xstatus_only();
			if (!(s->aux & CAPT_AUX_PAPER_DELIVERY))
				break;
			usleep(100000);
		}
	}
	return;
	capt_get_xstatus_only();
	capt_sendrecv(CAPT_CLEAR_ERROR, NULL, 0, NULL, 0);
	capt_sendrecv(CAPT_DISCARD_DATA, NULL, 0, NULL, 0);
	capt_get_xstatus_only();
	capt_sendrecv(CAPT_GO_OFFLINE, jbuf, 2, NULL, 0);
	capt_sendrecv(CAPT_RELEASE_UNIT, jbuf, 2, NULL, 0);
}

static void lbp2900_page_setup(struct printer_state_s *state,
		struct page_dims_s *dims,
		unsigned width, unsigned height)
{
	/* FIXME: Do we still need this function? */
	(void) state;
	(void) width;
	(void) height;
	(void) dims;
	/* Get raster dimensions straight from CUPS in paper.c */
	//dims->num_lines = dims->paper_height;
	//dims->line_size = dims->paper_width / 8;
	//dims->band_size = 70;
}

static void lbp2900_cancel_cleanup(struct printer_state_s *state)
{
	(void) state;
	uint8_t jbuf[2] = { LO(job), HI(job) };

	capt_sendrecv(CAPT_SET_LED_STATUS, lbp2900_gpio_init, ARRAY_SIZE(lbp2900_gpio_init), NULL, 0);
	send_job_start(CAPT_JOBFLAG_ABORT);
	capt_sendrecv(CAPT_RELEASE_UNIT, jbuf, 2, NULL, 0);
}

static void lbp3010_cancel_cleanup(struct printer_state_s *state)
{
	(void) state;
	uint8_t jbuf[2] = { LO(job), HI(job) };

	capt_sendrecv(CAPT_SET_LED_STATUS, lbp3010_gpio_init, ARRAY_SIZE(lbp3010_gpio_init), NULL, 0);
	send_job_start(CAPT_JOBFLAG_ABORT);
	capt_sendrecv(CAPT_RELEASE_UNIT, jbuf, 2, NULL, 0);
}

static void lbp2900_wait_user(struct printer_state_s *state)
{
	(void) state;

	capt_sendrecv(CAPT_SET_LED_STATUS, lbp2900_gpio_blink, ARRAY_SIZE(lbp2900_gpio_blink), NULL, 0);
	lbp2900_wait_ready(state->ops);

	while (1) {
		const struct capt_status_s *status = lbp2900_get_status(state->ops);
		if (FLAG(status, CAPT_FL_BUTTON_ON)) {
			fprintf(stderr, "DEBUG: CAPT: button activated\n");
		}
		if (FLAG(status, CAPT_FL_BUTTON)) {
			fprintf(stderr, "DEBUG: CAPT: button pressed\n");
			break;
		}
		usleep(100000);
	}

	capt_sendrecv(CAPT_SET_LED_STATUS, lbp2900_gpio_init, ARRAY_SIZE(lbp2900_gpio_init), NULL, 0);
	lbp2900_wait_ready(state->ops);
}

static void lbp3010_wait_user(struct printer_state_s *state)
{
	(void) state;

	capt_sendrecv(CAPT_SET_LED_STATUS, lbp3010_gpio_blink, ARRAY_SIZE(lbp3010_gpio_blink), NULL, 0);
	lbp2900_wait_ready(state->ops);

	while (1) {
		const struct capt_status_s *status = lbp2900_get_status(state->ops);
		if (FLAG(status, CAPT_FL_BUTTON_ON)) {
			fprintf(stderr, "DEBUG: CAPT: button activated\n");
		}
		if (FLAG(status, CAPT_FL_nERROR)) {
			fprintf(stderr, "DEBUG: CAPT: (virtual) button pressed\n");
			break;
		}
		sleep(1);
	}

	capt_sendrecv(CAPT_SET_LED_STATUS, lbp3010_gpio_init, ARRAY_SIZE(lbp3010_gpio_init), NULL, 0);
	lbp2900_wait_ready(state->ops);
}

static struct lbp2900_ops_s lbp2900_ops = {
	.ops = {
		.job_prologue = lbp3000_job_prologue,
		.job_epilogue = lbp2900_job_epilogue,
		.page_setup = lbp2900_page_setup,
		.page_prologue = lbp2900_page_prologue,
		.page_epilogue = lbp2900_page_epilogue,
		.compress_band = ops_compress_band_hiscoa,
		.send_band = ops_send_band_hiscoa,
		.cancel_cleanup = lbp2900_cancel_cleanup,
		.wait_user = lbp2900_wait_user,
	},
	.gpio = {
		.init = lbp2900_gpio_init,
		.blink = lbp2900_gpio_blink,
	},
	.get_status = capt_get_xstatus,
	.wait_ready = capt_wait_ready,
};
register_printer("LBP2900", lbp2900_ops.ops, WORKS);

static struct lbp2900_ops_s lbp3000_ops = {
	.ops = {
		.job_prologue = lbp3000_job_prologue,	/* different job prologue */
		.job_epilogue = lbp2900_job_epilogue,
		.page_setup = lbp2900_page_setup,
		.page_prologue = lbp2900_page_prologue,
		.page_epilogue = lbp2900_page_epilogue,
		.compress_band = ops_compress_band_hiscoa,
		.send_band = ops_send_band_hiscoa,
		.cancel_cleanup = lbp2900_cancel_cleanup,
		.wait_user = lbp2900_wait_user,
	},
	.gpio = {
		.init = lbp2900_gpio_init,
		.blink = lbp2900_gpio_blink,
	},
	.get_status = capt_get_xstatus,
	.wait_ready = capt_wait_ready,
};
register_printer("LBP3000", lbp3000_ops.ops, EXPERIMENTAL);

static struct lbp2900_ops_s lbp3010_ops = {
	.ops = {
		.job_prologue = lbp3010_job_prologue,
		.job_epilogue = lbp2900_job_epilogue,
		.page_setup = lbp2900_page_setup,
		.page_prologue = lbp2900_page_prologue,
		.page_epilogue = lbp2900_page_epilogue,
		.compress_band = ops_compress_band_hiscoa,
		.send_band = ops_send_band_hiscoa,
		.cancel_cleanup = lbp3010_cancel_cleanup,
		.wait_user = lbp3010_wait_user,
	},
	.gpio = {
		.init = lbp3010_gpio_init,
		.blink = lbp3010_gpio_blink,
	},
	.get_status = capt_get_xstatus_only,
	.wait_ready = capt_wait_xready_only,
};

static struct lbp2900_ops_s lbp6000_ops = {
	.ops = {
		.job_prologue = lbp6000_job_prologue,
		.job_epilogue = lbp2900_job_epilogue,
		.page_setup = lbp2900_page_setup,
		.page_prologue = lbp2900_page_prologue,
		.page_epilogue = lbp2900_page_epilogue,
		.compress_band = ops_compress_band_hiscoa,
		.send_band = ops_send_band_hiscoa,
		.cancel_cleanup = lbp3010_cancel_cleanup,
		.wait_user = lbp3010_wait_user,
	},
	.gpio = {
		.init = lbp3010_gpio_init,
		.blink = lbp3010_gpio_blink,
	},
	.get_status = capt_get_xstatus_only,
	.wait_ready = capt_wait_xready_only,
};

register_printer("LBP3010/LBP3018/LBP3050", lbp3010_ops.ops, WORKS);
register_printer("LBP3100/LBP3108/LBP3150", lbp3010_ops.ops, EXPERIMENTAL);
register_printer("LBP6000/LBP6018", lbp6000_ops.ops, EXPERIMENTAL);
