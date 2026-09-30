/*
 * Mobira R58 (RB58/RC58/RD58) logic board emulation: P8N or P8E CPU card,
 * A8N audio card, CU53AN or CU58AF handset.
 *
 * Time is kept in "xt" units = periods of the 8.064 MHz crystal.  P8E runs
 * the Z80 at 8.064 MHz with one wait state per M1 cycle; P8N at 4.032 MHz
 * without waits.  8254 CLK0/1 = 4.032 MHz, CLK2 and PIO A0 = 1968.75 Hz.
 */
#ifndef R58_H
#define R58_H

#include <stdint.h>
#include <stdio.h>

#include "z80.h"
#include "daisy.h"
#include "pio.h"
#include "sio.h"
#include "pit.h"
#include "cu53an.h"
#include "cu58af.h"

#define R58_XTAL_HZ   8064000.0
#define R58_NV_BASE   0xC000
#define R58_NV_SIZE   4096

enum { R58_P8E, R58_P8N };
enum { R58_CU53AN, R58_CU58AF };

/* ADC channels */
enum { AD_RSSI, AD_SQL, AD_BATT, AD_TPC, AD_FPM, AD_RPM, AD_TP4, AD_IN7 };

/* why r58_run() returned */
enum {
	R58_STOP_TIME = 0,	/* requested time elapsed */
	R58_STOP_BREAK,		/* breakpoint hit (PC in bp map) */
	R58_STOP_WATCH,		/* memory watchpoint written */
	R58_STOP_HALTED_OFF,	/* power relay dropped (radio off) */
};

/* a serial frame captured from OUT1 SD/CLK with a strobe */
typedef struct r58_frame {
	uint64_t bits;		/* bits in arrival order, first = MSB of nbits */
	int      nbits;
	uint8_t  strobe;	/* OUT1 strobe bit(s) that latched it */
	uint64_t at;		/* xt time */
} r58_frame;

typedef struct r58_synth {
	/* decoded register contents (see r58_synth_decode) */
	uint32_t rx_r, rx_n, rx_a;	/* RX PLL R, N, A */
	uint32_t tx_r, tx_n, tx_a;	/* TX PLL */
	uint8_t  ctrl;			/* control register (SCE) */
	uint16_t ext_a, ext_b;		/* external serial A (RAS) / B (TPS) */
	unsigned rx_loads, tx_loads, ctrl_loads;
} r58_synth;

/* events the host may want to see; kept in a small ring */
enum {
	R58_EV_WDRESET = 1,	/* watchdog reset the CPU */
	R58_EV_NMI,
	R58_EV_POWEROFF,	/* PB7 dropped the power relay */
	R58_EV_POWERON,
	R58_EV_TX_ON,		/* OUT1 TXOFF went 0 */
	R58_EV_TX_OFF,
	R58_EV_SYNTH,		/* synth register loaded; arg = strobe */
	R58_EV_MODEM_TX,	/* FX429 byte transmitted; arg = byte */
	R58_EV_MBUS_TX,		/* SIO B byte; arg = byte */
	R58_EV_GPS_TX,		/* SIO A byte; arg = byte */
	R58_EV_LCD,		/* display frame loaded */
};

typedef struct r58_event {
	uint64_t at;
	int      type;
	int      arg;
} r58_event;

#define R58_EVQ 1024
#define R58_TRACE 4096
#define R58_TXQ 4096

typedef struct r58 {
	/* configuration */
	int      card;			/* R58_P8E / R58_P8N */
	int      cu;			/* R58_CU53AN / R58_CU58AF */
	double   wd_timeout_s;		/* watchdog timeout */
	int      hook_offhook_level;	/* PA1 level meaning handset lifted */
	int      m1_wait;		/* wait states per M1 (P8E 1, P8N 0) */

	/* chips */
	z80      cpu;
	daisy    irq;
	pio      pio;
	sio      sio;
	pit      pit;
	cu53an   cu53;
	cu58af   cu58;

	/* memory */
	uint8_t  rom[0x10000];		/* EPROM0: 27C512 on P8E (32 KB used by FW) */
	uint8_t *rom1;			/* EPROM1: 27C010, 8 x 16 KB banks, or NULL */
	uint8_t  ram[0x4000];
	uint8_t  nvplane[R58_NV_SIZE];	/* P8N SMEM=0 plane */

	/* latches and converters */
	uint8_t  out0, out1, out2, csmem;
	uint8_t  da_rfc, da_txpwr;
	uint8_t  adc[8];		/* analog inputs, 0..255 */
	uint8_t  adc_result;
	uint8_t  multiboard;		/* value read at 0x80xx */

	/* inputs */
	uint8_t  power_on;		/* power switch */
	uint8_t  offhook;
	uint8_t  ccir_nibble;		/* PA7..4, 0xF idle */
	uint8_t  exin1, exin2, tmr0;	/* external levels on PB1, PB2, PB5 */
	uint8_t  ptt, local;		/* /PTT, /LOCAL pressed/grounded */
	uint8_t  modem_irq;		/* level toggled per modem event */

	/* power / reset state */
	uint8_t  powered;		/* relay on (CPU running) */
	uint64_t wd_last;		/* xt of last watchdog kick */

	/* time */
	uint64_t now;			/* xt */
	uint64_t pit01_rem;		/* xt carry for CLK0/1 (div 2) */
	uint64_t pit01_pending;		/* CLK0/1 clocks not yet given to the
					 * 8254 (r58.c pit01_sync) */
	uint64_t cpu_cycles;		/* T-states including waits */
	uint64_t instructions;

	/* OUT1 serial capture */
	uint64_t sbits;
	int      snbits;
	r58_frame last_frame;
	r58_synth synth;

	/* FX429 */
	uint8_t  mdm_ctrl, mdm_status;
	uint64_t mdm_txready_at;
	uint8_t  mdm_txbusy;
	uint8_t  mdm_rxq[64];
	int      mdm_rxn, mdm_rxpos;
	uint64_t mdm_rx_next;
	int      mdm_rx_state;		/* 0 idle, 1 sync pending, 2 bytes */

	/* serial host queues: bytes waiting to enter SIO A/B */
	uint8_t  rxq[2][R58_TXQ];
	int      rxq_r[2], rxq_w[2];
	uint64_t rx_next[2];

	/* events */
	r58_event ev[R58_EVQ];
	unsigned  ev_r, ev_w;

	/* debugging */
	uint8_t  bp[0x10000];		/* 1 = break when PC reaches it */
	uint8_t  wp[0x10000];		/* 1 = break when written */
	int      stop_reason;
	uint16_t stop_addr;
	uint16_t trace[R58_TRACE];
	unsigned trace_pos;
	int      bp_skip;		/* skip bp at current PC once */

	/* audio probe: edges of 8254 OUT1 (tone/PWM pin) */
	uint64_t *aud_t;
	uint8_t  *aud_v;
	unsigned  aud_n, aud_cap;

	/* NV persistence */
	char     nvfile[512];
} r58;

/* lifecycle */
void r58_init(r58 *m, int card, int cu);
int  r58_load_rom(r58 *m, const char *path);	/* EPROM0, up to 64 KB */
int  r58_load_rom1(r58 *m, const char *path);	/* EPROM1 (P8E), up to 128 KB */
int  r58_load_nv(r58 *m, const char *path);
int  r58_save_nv(r58 *m, const char *path);
void r58_power(r58 *m, int on);		/* flip the power switch */
void r58_reset(r58 *m);			/* hardware reset (power-up) */

/* run for at most `seconds` of emulated time; returns stop reason */
int  r58_run(r58 *m, double seconds);
double r58_time(const r58 *m);
void r58_step(r58 *m);			/* one instruction */
void r58_pit_sync(r58 *m);		/* lazy 8254 counters 0/1 up to now */

/* inputs */
void r58_set_adc(r58 *m, int ch, uint8_t v);
void r58_set_ptt(r58 *m, int pressed);
void r58_set_local(r58 *m, int grounded);
void r58_set_ign(r58 *m, int on);
void r58_set_hook(r58 *m, int offhook);
void r58_set_ccir(r58 *m, int nibble);
void r58_set_multiboard(r58 *m, uint8_t v);
/* key: firmware key char ('0'..'9','*','#','S','R','C','E','+','-','B'),
 * 0 = release all */
int  r58_key(r58 *m, int key);
void r58_serial_rx(r58 *m, int chan, const uint8_t *buf, int n);
/* queue an FSK packet (after sync) to arrive at the FX429 */
void r58_modem_rx(r58 *m, const uint8_t *buf, int n);

/* audio probe: record 8254 counter-1 output edges (the tone pin) */
void r58_audio_capture(r58 *m, unsigned capacity);	/* 0 = stop */
unsigned r58_audio_take(r58 *m, uint64_t *t, uint8_t *v, unsigned max);

/* observation */
int  r58_next_event(r58 *m, r58_event *e);
uint8_t r58_peek(r58 *m, uint16_t addr);
void r58_poke(r58 *m, uint16_t addr, uint8_t v);
/* decoded synthesizer frequency in Hz (VCO), -1 if unknown */
double r58_synth_vco_hz(const r58 *m, int tx, int prescaler, double tcxo_hz);
/* display: fills `upper`/`lower` with text (NUL-terminated) and returns
 * icon flags; renders CU53AN 7-segment or CU58AF 14-segment by reverse
 * font lookup (unknown glyphs as '?') */
int  r58_display_text(const r58 *m, char *upper, int ulen, char *lower, int llen);

#endif
