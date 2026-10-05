/*
 * Hitachi H8/532 in mode 3 (expanded maximum, no on-chip ROM, 8-bit
 * external bus): the H8/500 core plus the on-chip modules the R40 uses:
 * 1 KB RAM, ports 1, 7, 8, 9, FRT1-3, the 8-bit timer, PWM1-3 (registers
 * only), SCI (asynchronous), A/D, watchdog timer, interrupt controller,
 * wait-state controller.  The DTC is not modelled (nothing enables it).
 *
 * Source: H8/532 Hardware Manual (register bits and behaviour, bus
 * timing: 3 states per byte off-chip and in the register field plus the
 * WCR wait states off-chip, 2 states for on-chip RAM).
 *
 * Pins: the board pushes input levels with h8532_pin() (edges drive
 * input capture, IRQ1, the timer clock input) and sees outputs through
 * port_out.  External clocks on TMCI / FTCI are given as frequencies.
 */
#ifndef H8532_H
#define H8532_H

#include <stdint.h>

#include "h8500.h"

typedef struct h8532_bus {
	void    *ctx;
	uint8_t (*read)(void *ctx, uint32_t a);		/* off-chip */
	void    (*write)(void *ctx, uint32_t a, uint8_t v);
	/* pin levels of port p (1, 7, 9) changed: value driven by the chip on
	 * output pins, input levels elsewhere; ddr 1 = output */
	void    (*port_out)(void *ctx, int p, uint8_t pins, uint8_t ddr);
	/* A/D input ch 0..7, 0..1023 */
	int     (*adc)(void *ctx, int ch);
	/* SCI transmitted a frame; sent at the end of its stop bit */
	void    (*sci_tx)(void *ctx, uint8_t byte);
	/* TMO (P17) output level */
	void    (*tmo)(void *ctx, int level);
} h8532_bus;

typedef struct h8532_frt {
	uint8_t  tcr, tcsr, temp;
	uint16_t frc, ocra, ocrb, icr;
	uint32_t pre;		/* states toward the next count */
	uint64_t ext;		/* external clock accumulator */
} h8532_frt;

typedef struct h8532 {
	h8500    cpu;
	h8532_bus bus;
	double   phi;		/* Hz */

	uint8_t  iram[1024];	/* FB80-FF7F */
	uint8_t  pin[10];	/* input pin levels per port */
	uint8_t  ddr[10], dr[10];
	uint8_t  p1cr, wcr, ramcr, sbycr, ipr[4], dte[4];
	uint8_t  last_out[10];

	h8532_frt frt[3];
	double   ftci_hz[3];	/* external clock on FTCI1..3 */

	/* 8-bit timer */
	uint8_t  t_tcr, t_tcsr, t_tcora, t_tcorb, t_tcnt, tmo;
	uint32_t t_pre;
	uint64_t t_ext;
	double   tmci_hz;	/* clock on TMCI (P70) */

	uint8_t  pwm[3][3];	/* TCR DTR TCNT */

	/* SCI */
	uint8_t  smr, brr, scr, tdr, ssr, rdr;
	int      tx_busy;
	uint8_t  tsr;
	uint32_t tx_left;	/* states to the end of the frame in TSR */

	/* A/D */
	uint8_t  adcsr;
	uint16_t addr_[4];
	uint8_t  ad_temp;
	int      ad_ch;		/* channel being converted, -1 idle */
	uint32_t ad_left;

	/* WDT */
	uint8_t  w_tcsr, w_tcnt, w_pw;
	uint32_t w_pre;

	uint8_t  temp;		/* FRT/ADDR temporary register */

	/* interrupt sources latched by edge */
	uint8_t  nmi_req, irq1_req, wdt_irq0, nmi_level;

	uint64_t states;
} h8532;

void h8532_init(h8532 *m, const h8532_bus *bus, double phi);
void h8532_reset(h8532 *m);
/* one CPU instruction (or interrupt entry / sleep slice) and the modules
 * advanced by its states; returns states */
int  h8532_step(h8532 *m);
/* input pin level */
void h8532_pin(h8532 *m, int port, int bit, int level);
/* NMI pin level (edge per P1CR.NMIEG) */
void h8532_nmi_pin(h8532 *m, int level);
/* SCI frame received (asynchronous, already timed by the caller) */
void h8532_sci_rx(h8532 *m, uint8_t byte);
/* memory as the CPU sees it, no side effects on registers */
uint8_t h8532_peek(h8532 *m, uint32_t a);
void    h8532_poke(h8532 *m, uint32_t a, uint8_t v);
/* SCI bit rate in Hz for the current SMR/BRR */
double  h8532_sci_baud(const h8532 *m);

#endif
