/*
 * NEC uPD7810 / uPD78C10 CPU with its on-chip peripherals: ports A, B, C
 * (and D, F as plain latches), two 8-bit timers and the timer flip-flop,
 * the 16-bit timer/event counter, the asynchronous serial interface, the
 * 8-channel A/D converter, the AN4..AN7 edge detectors and the interrupt
 * controller.
 *
 * Instruction-stepped.  Time is kept in states (1 state = 3 clocks); the
 * instruction state counts and the skipped-instruction counts are the
 * uPD78C10A data sheet's (Feb 1995).  The data sheets in hand have no
 * mode-register bit tables; the bits modelled are the ones the TMx-1 and
 * HSN-2/HSF-2 firmware sets, read from their comments (notes/tmx1.md).
 */
#ifndef UPD7810_H
#define UPD7810_H

#include <stdint.h>

/* PSW */
#define UPD_CY 0x01
#define UPD_L0 0x04
#define UPD_L1 0x08
#define UPD_HC 0x10
#define UPD_SK 0x20
#define UPD_Z  0x40

/* interrupt / test flags, by their SKIT irf code */
enum {
	UPD_FNMI = 0, UPD_FT0, UPD_FT1, UPD_F1, UPD_F2, UPD_FE0, UPD_FE1,
	UPD_FEIN, UPD_FAD, UPD_FSR, UPD_FST, UPD_ER, UPD_OV,
	UPD_AN4 = 16, UPD_AN5, UPD_AN6, UPD_AN7, UPD_SB
};

enum { UPD_PA, UPD_PB, UPD_PC, UPD_PD, UPD_PF };

typedef struct upd7810_bus {
	void    *ctx;
	uint8_t (*read)(void *ctx, uint16_t a);
	void    (*write)(void *ctx, uint16_t a, uint8_t v);
	/* external level of the pins of port p (bits in input mode used) */
	uint8_t (*port_in)(void *ctx, int p);
	/* port p output latch or mode register written; mode bit 1 = input */
	void    (*port_out)(void *ctx, int p, uint8_t latch, uint8_t mode);
	/* A/D input ch (0..7), 0..255 */
	uint8_t (*adc)(void *ctx, int ch);
	/* serial TxD level changed */
	void    (*txd)(void *ctx, int level);
} upd7810_bus;

typedef struct upd7810 {
	uint8_t  v, a, b, c, d, e, h, l;
	uint8_t  v2, a2, b2, c2, d2, e2, h2, l2;
	uint16_t ea, ea2;
	uint16_t sp, pc;
	uint8_t  psw;
	uint8_t  ie;
	uint8_t  ei_delay;	/* EI takes effect after the next instruction */
	uint8_t  halt;		/* HLT: waits for an unmasked request */
	uint8_t  stopped;	/* STOP: waits for reset */
	uint8_t  illegal;	/* last instruction was undefined */
	uint16_t op_pc;		/* address of the instruction being executed */

	/* special registers */
	uint8_t  port[5];	/* output latches PA PB PC PD PF */
	uint8_t  mode[5];	/* MA MB MC (MD unused) MF, 1 = input */
	uint8_t  mcc, mm;
	uint8_t  mkl, mkh, anm, smh, sml, eom, etmm, tmm, zcm;
	uint8_t  tm0, tm1, txb, rxb, cr[4];
	uint16_t etm0, etm1, ecnt, ecpt;
	uint32_t irr;		/* request and test flags, bit = irf code */

	/* timers */
	uint8_t  cnt0, cnt1, tff;
	/* serial */
	uint8_t  txd;		/* TxD output level */
	uint8_t  rxd, ci;	/* RxD, CI input levels */
	uint8_t  txb_full, tx_busy, tx_tick, tx_nbits;
	uint16_t tx_shift;
	uint8_t  rx_state, rx_tick, rx_nbits, rx_full;
	uint16_t rx_shift;
	/* A/D */
	uint8_t  ad_idx;
	uint32_t ad_acc;

	uint64_t states;	/* since creation */
	uint8_t  iram[256];	/* FF00-FFFF when MM bit 3 (RAE) is set */
	upd7810_bus bus;
} upd7810;

void    upd7810_init(upd7810 *c, const upd7810_bus *bus);
void    upd7810_reset(upd7810 *c);
/* one instruction (or interrupt entry, or a HLT wait slice); returns states */
int     upd7810_step(upd7810 *c);
/* external events */
void    upd7810_set_rxd(upd7810 *c, int level);
void    upd7810_set_ci(upd7810 *c, int level);
void    upd7810_an_edge(upd7810 *c, int n);	/* falling edge on AN4+n */
void    upd7810_nmi(upd7810 *c);
uint8_t upd7810_port_read(upd7810 *c, int p);	/* as MOV A, PA etc. */
/* memory as the CPU sees it (internal RAM included), no side effects */
uint8_t upd7810_peek(upd7810 *c, uint16_t a);
void    upd7810_poke(upd7810 *c, uint16_t a, uint8_t v);
/* instruction length in bytes at a */
int     upd7810_oplen(upd7810 *c, uint16_t a);

#endif
