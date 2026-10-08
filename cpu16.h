/*
 * Motorola CPU16 core (the CPU of the 68HC16Z1).  The on-chip modules of
 * a particular chip (hc16z1.c) sit on the bus callbacks and present
 * interrupts through cpu16.irq_level / bus.iack.
 *
 * Instruction-stepped.  Time is kept in system clocks.  An instruction
 * costs its count from Table 6-36 of the CPU16 Reference Manual (which
 * assumes every access is a two-clock, 16-bit, aligned bus cycle) plus,
 * per access actually made, what the bus callback says the access takes
 * beyond those two clocks (Section 8.4: slower memories and 8-bit ports
 * only lengthen the program and operand accesses).  Program accesses are
 * charged one per instruction word, change-of-flow refills per Table 8-2.
 *
 * The architectural PC runs six bytes ahead of the executing instruction
 * (Section 7); the core keeps pc = address of the next instruction and
 * applies the +6 where it is visible: branch targets, stacked return
 * addresses (JSR/LBSR stack PC+6, BSR PC+4, exceptions next+6, SWI and
 * the other synchronous exceptions PC+8), RTS -2 and RTI -6.
 *
 * Sources: CPU16 Reference Manual (CPU16RM/AD rev 1) sections 3-9 and 11
 * and the instruction glossary; opcode table in cpu16tab.h, generated
 * from the manual's summary (tools/cpu16tab.py).
 */
#ifndef CPU16_H
#define CPU16_H

#include <stdint.h>

/* CCR */
#define C16_S	0x8000
#define C16_MV	0x4000
#define C16_H	0x2000
#define C16_EV	0x1000
#define C16_N	0x0800
#define C16_Z	0x0400
#define C16_V	0x0200
#define C16_C	0x0100
#define C16_IP	0x00E0	/* interrupt priority mask */
#define C16_SM	0x0010
/* CCR[3:0] is PK, kept in cpu16.pk */

/* exception vector numbers (vector address = 2 * n, bank 0) */
enum {
	C16_VEC_RESET = 0, C16_VEC_BKPT = 4, C16_VEC_BERR = 5, C16_VEC_SWI = 6,
	C16_VEC_ILLEGAL = 7, C16_VEC_ZERODIV = 8, C16_VEC_UNINIT = 0x0F,
	C16_VEC_AUTO = 0x10,	/* + level */
	C16_VEC_SPURIOUS = 0x18,
};

typedef struct cpu16_bus {
	void     *ctx;
	uint8_t  (*rd8)(void *ctx, uint32_t a);
	void     (*wr8)(void *ctx, uint32_t a, uint8_t v);
	/* aligned word accesses (a even) */
	uint16_t (*rd16)(void *ctx, uint32_t a);
	void     (*wr16)(void *ctx, uint32_t a, uint16_t v);
	/* clocks the bus cycles of one access take: size 1 (byte) or 2
	 * (aligned word; an 8-bit port takes two bus cycles for it) */
	int      (*clocks)(void *ctx, uint32_t a, int size);
	/* interrupt acknowledge cycle at level 1..7: the vector number */
	int      (*iack)(void *ctx, int level);
} cpu16_bus;

typedef struct cpu16 {
	uint16_t d, e, sp, pc;	/* pc: address of the next instruction */
	uint16_t r[3];		/* IX IY IZ */
	uint8_t  k[3];		/* XK YK ZK */
	uint8_t  ek, sk, pk;
	uint16_t ccr;		/* CCR[15:4]; [3:0] read as 0, PK is pk */
	uint16_t hr, ir;	/* MAC */
	int64_t  am;		/* 36-bit, sign-extended */
	uint8_t  xmsk, ymsk, sl;

	int      irq_level;	/* highest interrupt request presented, 0 none */
	uint8_t  irq_inhibit;	/* after ANDP/ORP/TAP/TDP: none before the next instruction */
	uint8_t  waiting;	/* WAI or LPSTOP: until an interrupt */
	uint8_t  stopped;	/* the wait is an LPSTOP (system clock stopped) */
	uint8_t  rmac;		/* RMAC in progress: the next step continues it */
	uint8_t  illegal;	/* last instruction was illegal (or BGND) */
	int      last_exc;	/* vector of the last exception taken, -1 none */
	uint32_t op_addr;	/* 20-bit address of the instruction being executed */

	uint64_t clocks;
	cpu16_bus bus;

	int      st;		/* per-instruction extra clocks */
} cpu16;

void     cpu16_init(cpu16 *c, const cpu16_bus *bus);
void     cpu16_reset(cpu16 *c);
/* one instruction (or RMAC iteration), interrupt entry or wait slice;
 * returns clocks */
int      cpu16_step(cpu16 *c);
/* 20-bit address of the next instruction */
uint32_t cpu16_pc20(const cpu16 *c);
/* instruction length at a (for stepping over breakpoints) */
int      cpu16_oplen(cpu16 *c, uint32_t a);
/* the full CCR, PK included */
uint16_t cpu16_ccr(const cpu16 *c);

#endif
