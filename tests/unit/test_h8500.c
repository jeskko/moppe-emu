/*
 * H8/500 core unit test: small hand-assembled programs (encodings from the
 * H8/500 Programming Manual, checked against the binutils h8500
 * disassembler), expected values worked out by hand.  A program starts at
 * 0x0100 in page 0 (maximum-mode reset vector) and ends at SLEEP.
 */
#include <stdio.h>
#include <string.h>

#include "../../h8500.h"

static uint8_t mem[0x20000];	/* pages 0 and 1 */
static int fails;
static h8500 c;

static uint8_t rd(void *x, uint32_t a) { (void)x; return mem[a & 0x1FFFF]; }
static void wr(void *x, uint32_t a, uint8_t v) { (void)x; mem[a & 0x1FFFF] = v; }
static int st(void *x, uint32_t a) { (void)x; (void)a; return 2; }

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

static void
load(const uint8_t *prog, size_t n)
{
	h8500_bus b = { 0, rd, wr, st };
	memset(mem, 0, sizeof(mem));
	mem[1] = 0x00; mem[2] = 0x01; mem[3] = 0x00;	/* reset: CP 0, PC 0100 */
	memcpy(mem + 0x100, prog, n);
	h8500_init(&c, &b);
	h8500_reset(&c);
}

static void
run(void)
{
	for (int i = 0; i < 100000 && !c.sleeping; i++)
		h8500_step(&c);
}

static int
flag(uint16_t f)
{
	return (c.sr & f) != 0;
}

/* MOV:I, MOV:E, ADD/SUB/CMP flags, Bcc */
static void
test_alu(void)
{
	static const uint8_t p[] = {
		0x58, 0x7F, 0xFF,	/* mov:i.w #7fff, r0 */
		0x59, 0x00, 0x01,	/* mov:i.w #0001, r1 */
		0xA9, 0x20,		/* add:g.w r1, r0      -> 8000, V N */
		0x1A,			/* sleep */
	};
	load(p, sizeof(p));
	run();
	CHECK(c.r[0] == 0x8000);
	CHECK(flag(H8_V) && flag(H8_N) && !flag(H8_C) && !flag(H8_Z));

	static const uint8_t q[] = {
		0x50, 0x10,		/* mov:e.b #10, r0 */
		0x51, 0x20,		/* mov:e.b #20, r1 */
		0xA1, 0x30,		/* sub.b r1, r0        -> f0, C N */
		0x52, 0x05,		/* mov:e.b #5, r2 */
		0x42, 0x05,		/* cmp:e.b #5, r2      -> Z */
		0x26, 0x02,		/* bne +2 (not taken) */
		0x53, 0x77,		/* mov:e.b #77, r3 */
		0x1A,
	};
	load(q, sizeof(q));
	c.r[0] = 0xAB00;
	run();
	CHECK(c.r[0] == 0xABF0);	/* byte ops keep the upper byte */
	CHECK((c.r[3] & 0xFF) == 0x77);	/* CMP set Z: BNE not taken */
}

/* shifts, ADD:Q, NEG, EXTS / EXTU, SWAP, XCH, DADD */
static void
test_misc(void)
{
	static const uint8_t p[] = {
		0x58, 0x81, 0x01,	/* mov:i.w #8101, r0 */
		0xA8, 0x1A,		/* shll.w r0           -> 0202, C */
		0x59, 0x00, 0x81,	/* mov:i.w #0081, r1 */
		0xA1, 0x19,		/* shar.b r1           -> c0, C */
		0xAA, 0x0D,		/* add:q.w #-2, r2     -> fffe */
		0x53, 0x01,		/* mov:e.b #1, r3 */
		0xA3, 0x14,		/* neg.b r3            -> ff */
		0x54, 0x80,		/* mov:e.b #80, r4 */
		0xA4, 0x11,		/* exts.b r4           -> ff80 */
		0x5D, 0x12, 0x34,	/* mov:i.w #1234, r5 */
		0xA5, 0x10,		/* swap r5             -> 3412 */
		0x1A,
	};
	load(p, sizeof(p));
	run();
	CHECK(c.r[0] == 0x0202);
	CHECK((c.r[1] & 0xFF) == 0xC0);
	CHECK(c.r[2] == 0xFFFE);
	CHECK((c.r[3] & 0xFF) == 0xFF);
	CHECK(c.r[4] == 0xFF80);
	CHECK(c.r[5] == 0x3412);

	static const uint8_t q[] = {
		0x58, 0x00, 0x11,	/* mov:i.w #0011, r0 */
		0x59, 0x00, 0x22,	/* mov:i.w #0022, r1 */
		0xA8, 0x91,		/* xch r0, r1 */
		0x52, 0x19,		/* mov:e.b #19, r2 */
		0x53, 0x28,		/* mov:e.b #28, r3 */
		0x0C, 0xFF, 0xFE, 0x58,	/* andc.w #fffe, sr (C = 0) */
		0xA3, 0x00, 0xA2,	/* dadd r3, r2         -> 47 */
		0x1A,
	};
	load(q, sizeof(q));
	run();
	CHECK(c.r[0] == 0x0022 && c.r[1] == 0x0011);
	CHECK((c.r[2] & 0xFF) == 0x47);
	CHECK(!flag(H8_C));
}

/* MULXU / DIVXU */
static void
test_muldiv(void)
{
	static const uint8_t p[] = {
		0x58, 0x00, 0x10,	/* mov:i.w #0010, r0 */
		0x59, 0x00, 0x20,	/* mov:i.w #0020, r1 */
		0xA1, 0xA8,		/* mulxu.b r1, r0      -> 0200 */
		0x5A, 0x12, 0x34,	/* mov:i.w #1234, r2 */
		0x5B, 0x01, 0x00,	/* mov:i.w #0100, r3 */
		0xAB, 0xAA,		/* mulxu.w r3, r2      -> r2:r3 = 0012 3400 */
		0x5C, 0x00, 0x00,	/* mov:i.w #0000, r4 */
		0x5D, 0x00, 0x64,	/* mov:i.w #0064, r5 (100) */
		0x5E, 0x00, 0x07,	/* mov:i.w #7, r6 */
		0xAE, 0xBC,		/* divxu.w r6, r4      -> r4 = 2 (rem), r5 = 14 */
		0x1A,
	};
	load(p, sizeof(p));
	run();
	CHECK(c.r[0] == 0x0200);
	CHECK(c.r[2] == 0x0012 && c.r[3] == 0x3400);
	CHECK(c.r[4] == 2 && c.r[5] == 14);
}

/* stack: STM / LDM, LINK / UNLK, BSR / RTS, PJSR / PRTS, byte push */
static void
test_stack(void)
{
	static const uint8_t p[] = {
		0x04, 0x00, 0x8F,	/* 0100 ldc.b #0, tp */
		0x5F, 0x80, 0x00,	/* 0103 mov:i.w #8000, r7 */
		0x58, 0x11, 0x11,	/* 0106 mov:i.w #1111, r0 */
		0x5A, 0x33, 0x33,	/* 0109 mov:i.w #3333, r2 */
		0x12, 0x05,		/* 010c stm (r0, r2), @-sp */
		0xA8, 0x13,		/* 010e clr.w r0 */
		0xAA, 0x13,		/* 0110 clr.w r2 */
		0x02, 0x05,		/* 0112 ldm @sp+, (r0, r2) */
		0x0E, 0x06,		/* 0114 bsr 011c */
		0x03, 0x01, 0x00, 0x00,	/* 0116 pjsr @01:0000 */
		0x1A,			/* 011a sleep */
		0x00,			/* 011b */
		0x17, 0xFC,		/* 011c link fp, #-4 */
		0x53, 0x5A,		/* 011e mov:e.b #5a, r3 */
		0xB7, 0x93,		/* 0120 mov:g.b r3, @-sp (a word slot) */
		0xC7, 0x84,		/* 0122 mov:g.b @sp+, r4 */
		0x0F,			/* 0124 unlk fp */
		0x19,			/* 0125 rts */
	};
	static const uint8_t far[] = {
		0x55, 0x99,		/* 01:0000 mov:e.b #99, r5 */
		0x11, 0x19,		/* prts */
	};
	load(p, sizeof(p));
	memcpy(mem + 0x10000, far, sizeof(far));
	run();
	CHECK(c.r[0] == 0x1111 && c.r[2] == 0x3333);
	CHECK((c.r[4] & 0xFF) == 0x5A);
	CHECK((c.r[5] & 0xFF) == 0x99);
	CHECK(c.r[7] == 0x8000);
	CHECK(c.cp == 0 && c.pc == 0x011B);
}

/* STC.B EP, @-SP / LDC.B @SP+, EP carry the EP:DP pair; @aa:8 uses BR */
static void
test_pages(void)
{
	static const uint8_t p[] = {
		0x04, 0x00, 0x8F,	/* ldc.b #0, tp */
		0x5F, 0x80, 0x00,	/* mov:i.w #8000, r7 */
		0x04, 0x12, 0x8C,	/* ldc.b #12, ep */
		0x04, 0x34, 0x8D,	/* ldc.b #34, dp */
		0xB7, 0x9C,		/* stc.b ep, @-sp */
		0x04, 0x00, 0x8C,	/* ldc.b #0, ep */
		0x04, 0x00, 0x8D,	/* ldc.b #0, dp */
		0xC7, 0x8C,		/* ldc.b @sp+, ep */
		0x04, 0x60, 0x8B,	/* ldc.b #60, br */
		0x50, 0xA5,		/* mov:e.b #a5, r0 */
		0x70, 0x10,		/* mov:s.b r0, @10:8   -> 6010 */
		0x1A,
	};
	load(p, sizeof(p));
	run();
	CHECK(c.ep == 0x12 && c.dp == 0x34);
	CHECK(mem[0x7FFE] == 0x12 && mem[0x7FFF] == 0x34);
	CHECK(mem[0x6010] == 0xA5);
	CHECK(c.r[7] == 0x8000);
}

/* bit ops, SCB/F loop */
static void
test_bits(void)
{
	static const uint8_t p[] = {
		0x05, 0x40, 0xC3,	/* bset.b #3, @40:8 */
		0x05, 0x40, 0xF3,	/* btst.b #3, @40:8    -> Z = 0 */
		0x58, 0x00, 0x04,	/* mov:i.w #4, r0 */
		0x59, 0x00, 0x00,	/* mov:i.w #0, r1 */
		0xA9, 0x08,		/* add:q.w #1, r1 */
		0x01, 0xB8, 0xFB,	/* scb/f r0, -5 */
		0x1A,
	};
	load(p, sizeof(p));
	c.br = 0;
	run();
	CHECK(mem[0x40] == 0x08);
	CHECK(c.r[1] == 5);
	CHECK(c.r[0] == 0xFFFF);
}

/* TRAPA / RTE frame, interrupt acceptance by the mask */
static void
test_exceptions(void)
{
	static const uint8_t p[] = {
		0x04, 0x00, 0x8F,	/* ldc.b #0, tp */
		0x5F, 0x80, 0x00,	/* mov:i.w #8000, r7 */
		0x08, 0x12,		/* trapa #2 */
		0x0C, 0xF8, 0xFF, 0x58,	/* andc.w #f8ff, sr (mask 0) */
		0x00, 0x00,		/* nop nop */
		0x1A,
	};
	load(p, sizeof(p));
	/* TRAPA #2 vector 0x48 -> 0200; IRQ vector 32 (0x80) -> 0300 */
	mem[0x49] = 0; mem[0x4A] = 0x02; mem[0x4B] = 0x00;
	mem[0x200] = 0x56; mem[0x201] = 0x42;	/* mov:e.b #42, r6 */
	mem[0x202] = 0x0A;			/* rte */
	mem[0x81] = 0; mem[0x82] = 0x03; mem[0x83] = 0x00;
	mem[0x300] = 0x55; mem[0x301] = 0x17;	/* mov:e.b #17, r5 */
	mem[0x302] = 0x0A;
	h8500_irq(&c, 32, 3);			/* masked until the ANDC */
	for (int i = 0; i < 6; i++)
		h8500_step(&c);
	CHECK((c.r[6] & 0xFF) == 0x42);
	CHECK((c.r[5] & 0xFF) == 0x00);		/* still masked */
	run();
	CHECK((c.r[5] & 0xFF) == 0x17);
	CHECK(c.r[7] == 0x8000);
	CHECK((c.sr & H8_I) == 0);		/* RTE restored the mask */
}

int
main(void)
{
	test_alu();
	test_misc();
	test_muldiv();
	test_stack();
	test_pages();
	test_bits();
	test_exceptions();
	if (fails) {
		printf("test_h8500: %d failures\n", fails);
		return 1;
	}
	printf("test_h8500: ok\n");
	return 0;
}
