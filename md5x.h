/*
 * Mobira Talkman MD50 / MD59 / ME59 logic board emulation: CDP1802
 * (MD50) or CDP1806 (MD59, ME59) processor module, audio/processor
 * board, CU53 or CU59 handset.
 *
 * Time is kept in CPU clock periods (MD5x 3.6864 MHz, ME59 4.8 MHz);
 * a machine cycle is 8 of them.  Sources: MD50 PE2/PE2A and AP1 service
 * texts, ME59NR service manual (PSA/ASA), and the hardware use of the
 * OH3NWQ (mx5x.asm) and OH1E (md50.asm) firmware; see notes/md5x.md.
 */
#ifndef MD5X_H
#define MD5X_H

#include <stdint.h>

#include "cdp1802.h"
#include "cu53an.h"
#include "pit.h"

enum { MD5X_MD50, MD5X_MD59, MD5X_ME59 };
enum { MD5X_CU53, MD5X_CU59 };

/* host-side inputs (logical: 1 = condition present) */
enum {
	MD5X_IN_POWEROFF,	/* power switch in the off position (/PW) */
	MD5X_IN_TOFF,		/* force TX fault: no output power even with XM */
	MD5X_IN_OFFHOOK,	/* handset lifted (/HK) */
	MD5X_IN_LOWBATT,	/* VC: supply below the VCS limit */
	MD5X_IN_PTT,		/* PTT: /EF4 and the model's PTT input bit */
	MD5X_IN_LOCAL,		/* service jumper */
	MD5X_IN_PORTABLE,	/* battery pack (POR / VRO) */
	MD5X_IN_COLDSTART,	/* POT: was off for more than 2 s */
	MD5X_IN_IGN,		/* ignition */
	MD5X_IN_AC,		/* ME59 AC (autocall) */
	MD5X_IN_N
};

enum {
	MD5X_STOP_TIME = 0,
	MD5X_STOP_BREAK,
	MD5X_STOP_WATCH,
	MD5X_STOP_OFF,
};

enum {
	MD5X_EV_WDRESET = 1,	/* watchdog pulled /CLEAR */
	MD5X_EV_POWEROFF,	/* OFF1 rising edge */
	MD5X_EV_POWERON,
	MD5X_EV_TX_ON,		/* XM set */
	MD5X_EV_TX_OFF,
	MD5X_EV_SYNTH,		/* PLL loaded; arg 0 = main (SE / RSE), 1 = ME59 offset (TSE) */
	MD5X_EV_MODEM_TX,	/* byte written to the modem; arg = byte */
	MD5X_EV_LCD,		/* display frame loaded */
	MD5X_EV_DAC,		/* ME59 AFC DAC written; arg = value */
	MD5X_EV_ILLEGAL,	/* undefined opcode; arg = PC */
};

typedef struct md5x_event {
	uint64_t at;
	int      type;
	int      arg;
} md5x_event;

typedef struct md5x_synth {
	/* main PLL: MD5x MC145156-style N (10 bits), A (7 bits), 2 switch
	 * bits; ME59 17-bit divider (128/129 prescaler) in 'div' */
	uint32_t n, a, div, sw;
	/* ME59 offset oscillator PLL (32/33 prescaler) */
	uint32_t off_n, off_a, off_sw;
	unsigned loads, off_loads;
	uint32_t bits;		/* shift register while SWE */
	int      nbits;
	int      last_nbits;	/* bits in the last strobed frame */
} md5x_synth;

typedef struct md5x_modem {
	uint8_t ctrl;
	uint8_t stat;		/* RFLAG HM TFLAG TXE FFSK CFLAG (GPIN live) */
	uint8_t rx, tx;
	uint64_t next_tick;	/* 100 Hz timer, CPU clocks */
	unsigned ticks;
	uint64_t tx_done;	/* TX shift register empties at this time */
} md5x_modem;

#define MD5X_EVQ 1024
#define MD5X_TRACE 4096

typedef struct md5x {
	int      model, cu;
	double   cpu_hz;
	cdp1802  cpu;
	uint64_t clk;		/* CPU clock periods since creation */
	int      powered;

	uint8_t  rom[32768];
	uint8_t  ram[8192];
	unsigned ram_size;
	uint8_t  prom[256];	/* MD50 74S287 ID PROM (4 bits) */
	int      prom_loaded;

	uint8_t  latch[8];	/* 4-bit addressable output latches */
	uint8_t  in[MD5X_IN_N];
	uint8_t  sr;		/* 4021 stages, bit 7 = Q8 (/EF3) */
	uint8_t  waiting;	/* MD50 N0: WAIT until /INT */

	md5x_modem modem;
	int      squelch_open;	/* MAS7205 GPIN2 on MD5x (SQ) */
	double   rx_tone_hz;	/* GPIN1: clipped RX audio, 0 = none */

	/* ME59 */
	uint8_t  adc[8], adc_result, dac;
	pit      pit;
	uint64_t pit_acc1, pit_acc2;
	double   if_hz;		/* 455 kHz IF into 8253 CLK1 */

	md5x_synth synth;
	cu53an   hs;

	/* watchdog: counts while WDR = 0 */
	uint64_t wd_since;
	double   wd_timeout_s;

	/* PHI (tone) edges */
	int      aud_on;
	int      aud_bit;	/* latch*4 + bit, default PHI = 1*4+2 */
	uint64_t *aud_t;
	uint8_t  *aud_v;
	unsigned aud_n, aud_cap;

	md5x_event ev[MD5X_EVQ];
	unsigned ev_head, ev_n;

	uint16_t trace[MD5X_TRACE];
	unsigned trace_pos;
	uint8_t  bp[65536 / 8];
	int      nbp;
	uint8_t  skip_bp;
	uint16_t watch_lo, watch_hi;
	int      watch_hit;
	uint16_t watch_addr;
} md5x;

void   md5x_init(md5x *m, int model, int cu, unsigned ram_size);
int    md5x_load_rom(md5x *m, const char *path);
void   md5x_power(md5x *m, int on);
int    md5x_run(md5x *m, double seconds);
int    md5x_step(md5x *m);
double md5x_time(const md5x *m);
void   md5x_set_input(md5x *m, int which, int v);
/* raw handset key code 0..31, bit 5 = the extra parallel-load bit, -1 none */
void   md5x_key(md5x *m, int code);
uint8_t md5x_peek(md5x *m, uint16_t a);
void   md5x_poke(md5x *m, uint16_t a, uint8_t v);
void   md5x_modem_rx(md5x *m, uint8_t byte);
int    md5x_event_pop(md5x *m, md5x_event *e);
void   md5x_audio_capture(md5x *m, int on);
void   md5x_breakpoint(md5x *m, uint16_t a, int on);

#endif
