/*
 * Hitachi H8/500 CPU core (maximum mode only: 24-bit addresses through
 * the CP/DP/EP/TP page registers, 4-byte vectors).  The on-chip
 * peripherals of a particular chip (h8532.c) sit on the bus callbacks and
 * present interrupts through h8500_irq().
 *
 * Instruction-stepped.  Time is kept in states (1 state = 1 clock of φ).
 * Every bus byte costs what the bus callback says (an 8-bit external bus
 * with wait states, or the on-chip registers), plus a per-instruction
 * internal count.  The internal counts are approximate (Table A-7 of the
 * H8/532 Hardware Manual less its fetch/operand part, rounded); nothing
 * on the R40 times itself by instruction loops (notes/r40.md).
 *
 * Sources: H8/500 Series Programming Manual (ADE-602-021) for encodings,
 * flags and exception handling; MAME's h8500 core (BSD-3-Clause, by R.
 * Belmont, AJR, O. Galibert) was read as a cross-check, notably for the
 * EP:DP pair moved by byte STC/LDC EP through @-R7/@R7+.
 */
#ifndef H8500_H
#define H8500_H

#include <stdint.h>

/* SR */
#define H8_C  0x0001
#define H8_V  0x0002
#define H8_Z  0x0004
#define H8_N  0x0008
#define H8_I  0x0700	/* interrupt mask I2..I0 */
#define H8_T  0x8000
#define H8_SR_MASK 0x870F

/* exception vector numbers (vector address = 4 * n in maximum mode) */
enum {
	H8_VEC_RESET = 0, H8_VEC_INVALID = 2, H8_VEC_ZERODIV = 3,
	H8_VEC_TRAPVS = 4, H8_VEC_ADDRERR = 8, H8_VEC_TRACE = 9,
	H8_VEC_NMI = 11, H8_VEC_TRAPA = 16, H8_VEC_IRQ = 32,
};

typedef struct h8500_bus {
	void    *ctx;
	uint8_t (*read)(void *ctx, uint32_t a);
	void    (*write)(void *ctx, uint32_t a, uint8_t v);
	/* states one byte access to a takes */
	int     (*states)(void *ctx, uint32_t a);
} h8500_bus;

typedef struct h8500 {
	uint16_t r[8];		/* R6 = FP, R7 = SP */
	uint16_t pc, sr;
	uint8_t  cp, dp, ep, tp, br;

	/* interrupt request presented by the chip: vector and level 1..7,
	 * level 8 for NMI (taken regardless of the mask); 0 = none */
	int      irq_vec, irq_level;
	uint8_t  irq_inhibit;	/* no interrupt before the next instruction */
	uint8_t  sleeping;
	uint8_t  illegal;	/* last instruction raised the invalid-instruction exception */
	int      last_exc;	/* vector of the last exception taken, -1 none */
	uint32_t op_addr;	/* address of the instruction being executed */

	uint64_t states;
	h8500_bus bus;

	/* per-instruction scratch */
	int      st;
	uint8_t  ea, ea1, ea2;
} h8500;

void     h8500_init(h8500 *c, const h8500_bus *bus);
void     h8500_reset(h8500 *c);
/* one instruction, interrupt entry or sleep slice; returns states */
int      h8500_step(h8500 *c);
/* set the highest-priority pending interrupt (0 vec = none) */
void     h8500_irq(h8500 *c, int vec, int level);
/* 24-bit address of the next instruction */
uint32_t h8500_pc24(const h8500 *c);
/* instruction length at a (for stepping over breakpoints) */
int      h8500_oplen(h8500 *c, uint32_t a);

#endif
