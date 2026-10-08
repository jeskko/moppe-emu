/*
 * Comarco MDR150 data radio (MDR Systems / Comarco Finland, logic board
 * by Telemic, 1994): 68HC16Z1 at 16.777 MHz (32.768 kHz crystal), 128 KB
 * Am29F010 flash on CSBOOT (read) / CS10 (write) with CE = ADDR18, 8-bit
 * port on D8..D15; 2 x 32 KB SRAM, 16-bit, read on CS2, writes on CS0
 * (D0..D7) and CS1 (D8..D15).  The radio module (PTV-01) is driven from
 * port F: a 4094 latch for its supplies and mute, an MB1504 synthesizer
 * (12.8 MHz reference) and the power level lines; its audio, RSSI, BUSY
 * and UNLOCK go to the ADC.  TX audio is the GPT's PWMA through an RC
 * filter.  The SCI goes through a 4053 to one of two RS-232 ports on the
 * D25 (SSEL = GP0), whose handshake lines are GPT and QSM port pins.
 * Six LEDs and a buzzer on port C.
 *
 * Wiring from the schematics (cdrom/, sheets 1-8), OH5NXO's notes in his
 * HaMDR tree (README, mdr_pinouts.txt, boot.s, pll_etc.s) and
 * notes/mdr150.md.
 */
#ifndef MDR150_H
#define MDR150_H

#include <stdint.h>

#include "hc16z1.h"

#define MDR_FLASH_SIZE	0x20000
#define MDR_RAM_SIZE	0x10000
#define MDR_PLL_REF	12800000.0

/* events */
enum {
	MDR_EV_ILLEGAL = 1,	/* arg: address */
	MDR_EV_EXC,		/* arg: vector */
	MDR_EV_RESET,		/* arg: why (HC16_RESET_*) */
	MDR_EV_SR,		/* 4094 latched; arg: outputs (Q8..Q1) */
	MDR_EV_PLL,		/* MB1504 latched; arg: total divide N*64+A, or R | 0x80000000 */
	MDR_EV_TX_ON,
	MDR_EV_TX_OFF,
	MDR_EV_LEDS,		/* arg: port C bits */
	MDR_EV_FLASH,		/* program or erase; arg: offset | 0x80000000 for erase */
	MDR_EV_UNMAPPED,	/* arg: address */
};

typedef struct mdr150_event {
	double  at;
	int     type;
	int     arg;
} mdr150_event;

/* serial ports on the D25: 0 = port 1 (A), 1 = port 2 (B) */
typedef struct mdr150_port {
	uint8_t  out[8192];		/* radio to host */
	int      out_head, out_tail;
	uint8_t  dtr, rts;		/* host's lines, asserted */
} mdr150_port;

typedef struct mdr150 {
	hc16z1   chip;

	uint8_t  flash[MDR_FLASH_SIZE];
	uint8_t  ram[MDR_RAM_SIZE];
	int      flash_cycle;		/* command state */
	int      flash_mode;		/* 0 read, 1 program next, 2 autoselect */
	int      flash_dirty;

	/* radio module */
	uint8_t  pf;			/* port F as driven */
	uint8_t  sr_shift, sr_out;	/* 4094 */
	uint32_t pll_shift;
	uint32_t pll_na, pll_r;		/* MB1504 latches */
	int      tx_keyed;
	int      power_level;		/* POWM:POWL, 3 = lowest */

	/* front end */
	int      rssi;			/* ADC counts on AN1 */
	int      busy;			/* BUSY line level */
	int      audio_level;		/* peak counts of RX audio on the AI inputs */
	int      noise_level;		/* peak counts of noise when no signal */
	const float *rx_audio;		/* signal on the channel, -1..1 */
	int      rx_len;
	double   rx_rate, rx_start;	/* sample rate, start time (s) */
	uint32_t noise_seed;

	/* TX audio: PWMA duty changes while transmitting */
	uint64_t *pwm_at;
	int16_t  *pwm_duty;
	int      pwm_n, pwm_cap;

	/* serial */
	mdr150_port port[2];
	int      ssel;

	uint8_t  leds;			/* port C */

	/* debugging */
	mdr150_event ev[4096];
	int      ev_head, ev_tail;
	uint8_t *bp;			/* breakpoint bitmap, 1 MB of addresses */
	uint32_t watch_lo, watch_hi, watch_addr;
	int      watch_hit;
	uint32_t trace[256];
	int      trace_n;
	int      powered;
	int      log_exc;		/* log MDR_EV_EXC (interrupts are thousands a second) */
} mdr150;

void   mdr150_init(mdr150 *r);
void   mdr150_free(mdr150 *r);
int    mdr150_load_flash(mdr150 *r, const uint8_t *img, int n);
void   mdr150_power(mdr150 *r, int on);
/* run for s seconds of radio time; returns 0 time, 1 breakpoint, 2 watch */
int    mdr150_run(mdr150 *r, double s);
int    mdr150_step(mdr150 *r);
double mdr150_time(const mdr150 *r);
/* the RX audio on the channel: n samples at rate from now (the radio
 * hears them while its receiver is on); NULL to stop */
void   mdr150_set_rx_audio(mdr150 *r, const float *x, int n, double rate);
void   mdr150_serial_in(mdr150 *r, int port, uint8_t b);
double mdr150_vco_hz(const mdr150 *r);

#endif
