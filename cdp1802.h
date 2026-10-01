/*
 * RCA CDP1802 CPU core, with the CDP1804/1805/1806 extensions (68xx
 * instructions, counter/timer, XIE/CIE interrupt enables) when built as
 * CDP1806.
 *
 * Instruction-stepped.  Each machine cycle is 8 clocks; every 1802
 * instruction takes 2 machine cycles except the long branches and skips
 * (0xCx, incl. NOP), which take 3.  The 68xx instructions take 3-10.
 * Interrupt entry takes 1, an IDL state 1 per cycle until an interrupt.
 *
 * I/O: OUT n puts M(R(X)) on the bus and INP n writes the bus into
 * M(R(X)) and D; both pass the R(X) address to the callback because the
 * boards decode register numbers from the low address bits.
 */
#ifndef CDP1802_H
#define CDP1802_H

#include <stdint.h>

enum { CDP1802, CDP1806 };

/* counter/timer modes (1805/1806) */
enum {
	CDP_CT_STOP = 0,
	CDP_CT_EVENT1,		/* EF1 high-to-low decrements */
	CDP_CT_EVENT2,		/* EF2 high-to-low */
	CDP_CT_TIMER,		/* TPA / 32 */
	CDP_CT_PULSE1,		/* TPA while EF1 low; stops on EF1 rising */
	CDP_CT_PULSE2,
};

typedef struct cdp1802 cdp1802;

struct cdp1802 {
	uint16_t r[16];
	uint8_t  p, x, d, df, t, ie, q;
	uint8_t  idle;		/* IDL executed, waiting for an interrupt */
	uint8_t  type;		/* CDP1802 or CDP1806 */

	/* 1805/1806 counter/timer and interrupt enables */
	uint8_t  cntr, ch;	/* counter, holding register */
	uint8_t  ctmode;	/* CDP_CT_* */
	uint8_t  prescale;	/* divide-by-32 for timer mode */
	uint8_t  cil;		/* counter interrupt latch */
	uint8_t  cie, xie;	/* counter / external interrupt enable */
	uint8_t  etq;		/* Q toggles on counter underflow */
	uint8_t  ef_last;	/* EF1/EF2 pin state for the event/pulse modes */

	/* inputs */
	uint8_t  int_line;	/* /INT asserted (level) */

	/* last step */
	unsigned cycles;	/* machine cycles */
	uint8_t  illegal;	/* an undefined 68xx opcode was executed */

	/* bus callbacks */
	void    *ctx;
	uint8_t (*read)(void *ctx, uint16_t addr);
	void    (*write)(void *ctx, uint16_t addr, uint8_t val);
	/* n = 1..7; addr = R(X) */
	uint8_t (*in)(void *ctx, int n, uint16_t addr);
	void    (*out)(void *ctx, int n, uint16_t addr, uint8_t val);
	/* flag inputs, bit k = EF(k+1) true (pin low) */
	uint8_t (*ef)(void *ctx);
	/* Q output changed; may be NULL */
	void    (*qout)(void *ctx, int level);
};

/* /CLEAR: X, P, R0, Q = 0, IE = 1 (and XIE, CIE; counter stopped) */
void     cdp1802_reset(cdp1802 *c, int type);
/* Execute one instruction, an interrupt entry or an idle cycle.
 * Returns machine cycles (also in c->cycles). */
unsigned cdp1802_step(cdp1802 *c);

#endif
