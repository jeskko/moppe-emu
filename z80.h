/*
 * Z80 CPU core, instruction-stepped with exact T-state counts.
 *
 * Covers the full documented and undocumented instruction set (IXH/IXL,
 * SLL, DDCB register copies, undocumented X/Y flags incl. MEMPTR/WZ for
 * BIT n,(HL)).  Tracks the number of M1 (opcode fetch) cycles per step so
 * the system can add wait states: the R58 P8E card inserts one wait state
 * on every M1.
 */
#ifndef Z80_H
#define Z80_H

#include <stdint.h>

typedef struct z80 z80;

struct z80 {
	uint8_t  a, f, b, c, d, e, h, l;
	uint8_t  a_, f_, b_, c_, d_, e_, h_, l_;
	uint16_t ix, iy, sp, pc;
	uint16_t wz;		/* MEMPTR, undocumented */
	uint8_t  i, r;		/* r: all 8 bits, low 7 count M1s */
	uint8_t  iff1, iff2, im;
	uint8_t  halted;
	uint8_t  ei_pending;	/* EI delays interrupt acceptance by one insn */
	uint8_t  q, lastq;	/* F as written by this/last insn (SCF/CCF X/Y) */

	/* interrupt inputs */
	uint8_t  int_line;	/* level: /INT asserted */
	uint8_t  nmi_pending;	/* edge latched by the system */

	/* per-step counters */
	unsigned m1;		/* M1 cycles in the last step */
	unsigned t;		/* T-states of the last step */

	/* bus callbacks */
	void    *ctx;
	uint8_t (*read)(void *ctx, uint16_t addr);
	void    (*write)(void *ctx, uint16_t addr, uint8_t val);
	uint8_t (*in)(void *ctx, uint16_t port);
	void    (*out)(void *ctx, uint16_t port, uint8_t val);
	/* interrupt acknowledge: returns data bus byte (IM2 vector low byte) */
	uint8_t (*int_ack)(void *ctx);
	/* RETI decoded (ED 4D), for Z80-family daisy chain; may be NULL */
	void    (*reti)(void *ctx);
};

void     z80_reset(z80 *z);
/* Execute one instruction (or accept an interrupt). Returns T-states,
 * not including external wait states; z->m1 holds the M1 count. */
unsigned z80_step(z80 *z);

/* flag bits */
#define Z80_CF 0x01
#define Z80_NF 0x02
#define Z80_PF 0x04
#define Z80_XF 0x08
#define Z80_HF 0x10
#define Z80_YF 0x20
#define Z80_ZF 0x40
#define Z80_SF 0x80

#endif
