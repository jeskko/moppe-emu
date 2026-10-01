/*
 * CDP1802/1806 core unit test: small hand-assembled programs, expected
 * values worked out from MPM-201A and the CDP1805/1806 data sheet.
 */
#include <stdio.h>
#include <string.h>

#include "../../cdp1802.h"

static uint8_t mem[65536];
static int fails;
static uint16_t io_addr[8];
static uint8_t io_val[8], in_val = 0x5a, efs;
static int qedges;

static uint8_t rd(void *x, uint16_t a) { (void)x; return mem[a]; }
static void wr(void *x, uint16_t a, uint8_t v) { (void)x; mem[a] = v; }
static uint8_t in(void *x, int n, uint16_t a) { (void)x; io_addr[n] = a; return in_val; }
static void out(void *x, int n, uint16_t a, uint8_t v) { (void)x; io_addr[n] = a; io_val[n] = v; }
static uint8_t ef(void *x) { (void)x; return efs; }
static void qo(void *x, int l) { (void)x; (void)l; qedges++; }

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

static cdp1802 c;

/* load program at 0, reset, run until IDL; returns total machine cycles */
static unsigned
run(int type, const uint8_t *prog, size_t n)
{
	memset(mem, 0, sizeof(mem));
	memcpy(mem, prog, n);
	memset(&c, 0, sizeof(c));
	c.read = rd; c.write = wr; c.in = in; c.out = out; c.ef = ef; c.qout = qo;
	cdp1802_reset(&c, type);
	unsigned cyc = 0;
	for (int i = 0; i < 100000 && !c.idle; i++)
		cyc += cdp1802_step(&c);
	return cyc;
}

#define RUN(type, ...) do { static const uint8_t p_[] = { __VA_ARGS__ }; run(type, p_, sizeof(p_)); } while (0)

int
main(void)
{
	/* LDI 0x80; ADI 0x90 -> D 0x10, DF 1 */
	RUN(CDP1802, 0xf8, 0x80, 0xfc, 0x90, 0x00);
	CHECK(c.d == 0x10 && c.df == 1);
	/* SMI: 0x10 - 0x20 = 0xF0, DF 0 (borrow) */
	RUN(CDP1802, 0xf8, 0x10, 0xff, 0x20, 0x00);
	CHECK(c.d == 0xf0 && c.df == 0);
	/* SDI: 0x20 - 0x10 = 0x10, DF 1 */
	RUN(CDP1802, 0xf8, 0x10, 0xfd, 0x20, 0x00);
	CHECK(c.d == 0x10 && c.df == 1);
	/* SMBI with DF 0 subtracts one more: LDI 5; SMI 6 (DF 0, D FF); SMBI 0 -> FE, DF 1 */
	RUN(CDP1802, 0xf8, 0x05, 0xff, 0x06, 0x7f, 0x00, 0x00);
	CHECK(c.d == 0xfe && c.df == 1);
	/* ADCI with DF 1: LDI FF; ADI 1 (D 0, DF 1); ADCI 0 -> 1, DF 0 */
	RUN(CDP1802, 0xf8, 0xff, 0xfc, 0x01, 0x7c, 0x00, 0x00);
	CHECK(c.d == 0x01 && c.df == 0);
	/* SDBI: DF 0: LDI 5; SMI 6 -> DF 0; LDI 3; SDBI 10 -> 10-3-1 = 6, DF 1 */
	RUN(CDP1802, 0xf8, 0x05, 0xff, 0x06, 0xf8, 0x03, 0x7d, 0x0a, 0x00);
	CHECK(c.d == 0x06 && c.df == 1);

	/* shifts: LDI 0x81; SHR -> 0x40 DF1; SHRC -> 0xA0 DF0; SHLC -> 0x40 DF1; SHL -> 0x80 DF0 */
	RUN(CDP1802, 0xf8, 0x81, 0xf6, 0x00);
	CHECK(c.d == 0x40 && c.df == 1);
	RUN(CDP1802, 0xf8, 0x81, 0xf6, 0x76, 0x00);
	CHECK(c.d == 0xa0 && c.df == 0);
	RUN(CDP1802, 0xf8, 0x81, 0x7e, 0x00);
	CHECK(c.d == 0x02 && c.df == 1);
	RUN(CDP1802, 0xf8, 0x41, 0xfe, 0x00);
	CHECK(c.d == 0x82 && c.df == 0);

	/* register ops: LDI 12 PHI 5 LDI 34 PLO 5 INC 5 GHI 5 -> D 12, R5 1235 */
	RUN(CDP1802, 0xf8, 0x12, 0xb5, 0xf8, 0x34, 0xa5, 0x15, 0x95, 0x00);
	CHECK(c.r[5] == 0x1235 && c.d == 0x12);

	/* memory via X: R6 = 0x0100; SEX 6; LDI 7; STXD -> M(100)=7, R6=FF;
	 * IRX; LDXA -> D 7, R6 101; ADD with M(101)=0 */
	RUN(CDP1802, 0xf8, 0x01, 0xb6, 0xf8, 0x00, 0xa6, 0xe6, 0xf8, 0x07,
	    0x73, 0x60, 0x72, 0x00);
	CHECK(mem[0x100] == 7 && c.r[6] == 0x101 && c.d == 7);

	/* short branches: LDI 0; BZ 6; LDI 1; IDL @5 ; @6 LDI 2; IDL */
	RUN(CDP1802, 0xf8, 0x00, 0x32, 0x06, 0xf8, 0x01, 0xf8, 0x02, 0x00);
	CHECK(c.d == 2);
	/* BNZ not taken, falls through */
	RUN(CDP1802, 0xf8, 0x00, 0x3a, 0x06, 0xf8, 0x01, 0x00, 0x00, 0x00);
	CHECK(c.d == 1);
	/* SKP skips one byte: SKP; 0x00(skipped); LDI 9 */
	RUN(CDP1802, 0x38, 0x00, 0xf8, 0x09, 0x00);
	CHECK(c.d == 9);
	/* long branch LBR 0x1234 */
	{
		static const uint8_t p[] = { 0xc0, 0x12, 0x34 };
		memset(mem, 0, sizeof(mem));
		run(CDP1802, p, sizeof(p));
		/* mem at 0x1234 is 0 = IDL, run stopped there */
		CHECK(c.r[0] == 0x1235);
	}
	/* long skips: LDI 0; LSZ; LDI 1 (skipped, 2 bytes); LDI 3 */
	RUN(CDP1802, 0xf8, 0x00, 0xce, 0xf8, 0x01, 0xf8, 0x03, 0x00);
	CHECK(c.d == 3);
	/* LSNZ with D=0 does not skip */
	RUN(CDP1802, 0xf8, 0x00, 0xc6, 0xf8, 0x01, 0x00);
	CHECK(c.d == 1);
	/* EF: B3 taken when EF3 true */
	efs = 4;
	RUN(CDP1802, 0x36, 0x05, 0xf8, 0x01, 0x00, 0xf8, 0x02, 0x00);
	CHECK(c.d == 2);
	efs = 0;
	RUN(CDP1802, 0x36, 0x05, 0xf8, 0x01, 0x00, 0xf8, 0x02, 0x00);
	CHECK(c.d == 1);

	/* SEQ/REQ and BQ */
	qedges = 0;
	RUN(CDP1802, 0x7b, 0x31, 0x05, 0x00, 0x00, 0x7a, 0x00);
	CHECK(c.q == 0 && qedges == 2);

	/* cycle counts: LDI(2) + LBR(3) + NOP(3) + IDL(2) */
	{
		static const uint8_t p[] = { 0xf8, 0x00, 0xc0, 0x00, 0x05, 0xc4, 0x00 };
		unsigned cyc = run(CDP1802, p, sizeof(p));
		CHECK(cyc == 2 + 3 + 3 + 2);
	}

	/* OUT/INP pass R(X): R7 = 0x8006; SEX 7; OUT 4 -> addr 8006, R7 8007; INP 2 -> M(8007), D */
	{
		static const uint8_t p[] = { 0xf8, 0x80, 0xb7, 0xf8, 0x06, 0xa7, 0xe7,
		                             0x64, 0x6a, 0x00 };
		memset(mem, 0, sizeof(mem));
		mem[0x8006] = 0x3c;
		memcpy(mem, p, sizeof(p));
		memset(&c, 0, sizeof(c));
		c.read = rd; c.write = wr; c.in = in; c.out = out; c.ef = ef;
		cdp1802_reset(&c, CDP1802);
		while (!c.idle)
			cdp1802_step(&c);
		CHECK(io_addr[4] == 0x8006 && io_val[4] == 0x3c);
		CHECK(io_addr[2] == 0x8007 && mem[0x8007] == 0x5a && c.d == 0x5a);
		CHECK(c.r[7] == 0x8007);
	}

	/* MARK/SAV/RET: X=0 P=0 at reset. R2 = 0x200. SEX 3? keep simple:
	 * MARK -> T = 0x00, M(200) = 00, X = P = 0, R2 = 1FF */
	RUN(CDP1802, 0xf8, 0x02, 0xb2, 0xf8, 0x00, 0xa2, 0x79, 0x00);
	CHECK(c.r[2] == 0x1ff && mem[0x200] == 0 && c.x == 0);

	/* DIS/RET: SEX 0 (R0 = PC); DIS; .byte 0x13 -> X 1, P 3, IE 0 */
	{
		static const uint8_t p[] = { 0x71, 0x13 };
		memset(mem, 0, sizeof(mem));
		memcpy(mem, p, sizeof(p));
		memset(&c, 0, sizeof(c));
		c.read = rd; c.write = wr; c.ef = ef;
		cdp1802_reset(&c, CDP1802);
		c.r[3] = 0x300;
		mem[0x300] = 0xf8; mem[0x301] = 0x44; mem[0x302] = 0x00;
		while (!c.idle)
			cdp1802_step(&c);
		CHECK(c.ie == 0 && c.x == 1 && c.p == 3 && c.d == 0x44);
	}

	/* interrupt entry: IE 1 at reset, /INT -> T = (X,P), X 2, P 1, IE 0, 1 cycle */
	{
		memset(mem, 0, sizeof(mem));
		memset(&c, 0, sizeof(c));
		c.read = rd; c.write = wr; c.ef = ef;
		cdp1802_reset(&c, CDP1802);
		c.p = 3; c.x = 5;
		c.int_line = 1;
		unsigned cyc = cdp1802_step(&c);
		CHECK(cyc == 1 && c.t == 0x53 && c.x == 2 && c.p == 1 && c.ie == 0);
		/* IDL is left on interrupt */
		c.int_line = 0;
		c.r[1] = 0x400; mem[0x400] = 0x00;
		cdp1802_step(&c);
		CHECK(c.idle);
		cdp1802_step(&c);
		CHECK(c.idle);
		c.int_line = 1;
		cdp1802_step(&c);
		CHECK(!c.idle);
	}

	/* ---- 1806 ---- */

	/* RLDI R9,0x1234; RNX? : 68 C9 12 34 -> R9 */
	RUN(CDP1806, 0x68, 0xc9, 0x12, 0x34, 0x00);
	CHECK(c.r[9] == 0x1234);
	/* DBNZ: RLDI R8,3; loop: DBNZ R8,loop(=4); IDL -> R8 0 */
	{
		static const uint8_t p[] = { 0x68, 0xc8, 0x00, 0x03, 0x68, 0x28, 0x00, 0x04, 0x00 };
		unsigned cyc = run(CDP1806, p, sizeof(p));
		CHECK(c.r[8] == 0 && cyc == 5 + 3 * 5 + 2);
	}
	/* SCAL/SRET: R2 stack at 0x200; RLDI R2,0200; SEX 2; SCAL R4,0x0010; IDL
	 * @0x10: LDI 7; SRET R4 */
	{
		static const uint8_t p[] = { 0x68, 0xc2, 0x02, 0x00, 0xe2,
		                             0x68, 0x84, 0x00, 0x10, 0x00 };
		memset(mem, 0, sizeof(mem));
		memcpy(mem, p, sizeof(p));
		mem[0x10] = 0xf8; mem[0x11] = 0x07; mem[0x12] = 0x68; mem[0x13] = 0x94;
		memset(&c, 0, sizeof(c));
		c.read = rd; c.write = wr; c.ef = ef;
		cdp1802_reset(&c, CDP1806);
		c.r[4] = 0xabcd;
		unsigned cyc = 0;
		while (!c.idle)
			cyc += cdp1802_step(&c);
		CHECK(c.d == 7 && c.r[0] == 0x000a && c.r[4] == 0xabcd && c.r[2] == 0x200);
		CHECK(mem[0x200] == 0xcd && mem[0x1ff] == 0xab);
		CHECK(cyc == 5 + 2 + 10 + 2 + 8 + 2);
	}
	/* RSXD/RLXA round trip: RLDI R2,0200; SEX 2; RLDI R6,BEEF; RSXD R6;
	 * IRX; RLXA R7 -> R7 BEEF, R2 0201 */
	RUN(CDP1806, 0x68, 0xc2, 0x02, 0x00, 0xe2, 0x68, 0xc6, 0xbe, 0xef,
	    0x68, 0xa6, 0x60, 0x68, 0x67, 0x00);
	CHECK(c.r[7] == 0xbeef && c.r[2] == 0x201);
	/* DSAV: T, D, D>>1 with carry. RLDI R2,0200; SEX 2; LDI 0x81; SHL(D 02 DF1); LDI 0x03;
	 * DSAV -> M(1FF)=T, M(1FE)=03, D = 0x81 (03>>1 | DF<<7), M(1FD)=0x81, DF=1 */
	RUN(CDP1806, 0x68, 0xc2, 0x02, 0x00, 0xe2, 0xf8, 0x81, 0xfe, 0xf8, 0x03,
	    0x68, 0x76, 0x00);
	CHECK(mem[0x1fe] == 0x03 && mem[0x1fd] == 0x81 && c.d == 0x81 && c.df == 1 && c.r[2] == 0x1fd);
	/* BCD: LDI 0x38; DADI 0x45 -> 0x83, DF 0; LDI 0x99; DADI 0x01 -> 0x00, DF 1 */
	RUN(CDP1806, 0xf8, 0x38, 0x68, 0xfc, 0x45, 0x00);
	CHECK(c.d == 0x83 && c.df == 0);
	RUN(CDP1806, 0xf8, 0x99, 0x68, 0xfc, 0x01, 0x00);
	CHECK(c.d == 0x00 && c.df == 1);
	/* DSMI: 0x42 - 0x15 = 0x27 DF 1; 0x15 - 0x42 = 0x73 DF 0 */
	RUN(CDP1806, 0xf8, 0x42, 0x68, 0xff, 0x15, 0x00);
	CHECK(c.d == 0x27 && c.df == 1);
	RUN(CDP1806, 0xf8, 0x15, 0x68, 0xff, 0x42, 0x00);
	CHECK(c.d == 0x73 && c.df == 0);

	/* counter: DIS first, LDI 2; LDC; STM; loop: BCI out; BR loop; out: GEC; IDL
	 * underflow after 2 * 32 TPA */
	{
		static const uint8_t p[] = {
			0x71, 0x00,		/* 0 DIS */
			0xf8, 0x02,		/* 2 LDI 2 */
			0x68, 0x06,		/* 4 LDC */
			0x68, 0x07,		/* 6 STM */
			0x68, 0x3e, 0x0d,	/* 8 BCI 0d */
			0x30, 0x08,		/* b BR 08 */
			0x68, 0x08,		/* d GEC */
			0x00,
		};
		unsigned cyc = run(CDP1806, p, sizeof(p));
		/* counter reloads to 2 after 01 */
		CHECK(c.d == 2 && c.cil == 0);
		CHECK(cyc >= 64 && cyc < 64 + 20);
	}
	/* counter interrupt: CIE, MIE set; interrupt entry when CI latched */
	{
		static const uint8_t p[] = {
			0xf8, 0x01,		/* LDI 1 */
			0x68, 0x06,		/* LDC */
			0x68, 0x07,		/* STM */
			0x30, 0x06,		/* BR . */
		};
		memset(mem, 0, sizeof(mem));
		memcpy(mem, p, sizeof(p));
		memset(&c, 0, sizeof(c));
		c.read = rd; c.write = wr; c.ef = ef;
		cdp1802_reset(&c, CDP1806);
		int i;
		for (i = 0; i < 200 && c.p != 1; i++)
			cdp1802_step(&c);
		CHECK(c.p == 1 && c.x == 2 && c.ie == 0 && c.t == 0x00);
	}
	/* ETQ toggles Q on each underflow */
	{
		static const uint8_t p[] = {
			0x71, 0x00, 0xf8, 0x01, 0x68, 0x06, 0x68, 0x09, 0x68, 0x07,
			0x30, 0x0a,
		};
		memset(mem, 0, sizeof(mem));
		memcpy(mem, p, sizeof(p));
		memset(&c, 0, sizeof(c));
		c.read = rd; c.write = wr; c.ef = ef; c.qout = qo;
		cdp1802_reset(&c, CDP1806);
		qedges = 0;
		unsigned cyc = 0;
		while (qedges < 5 && cyc < 10000)
			cyc += cdp1802_step(&c);
		/* timer runs from STM (cycle ~10): 5 underflows of a count of 1 */
		CHECK(c.q == 1 && cyc >= 10 + 5 * 32 - 4 && cyc <= 10 + 5 * 32 + 4);
	}
	/* 1802 ignores the 68 prefix as illegal */
	RUN(CDP1802, 0x68, 0x00);
	CHECK(c.d == 0);

	if (fails) {
		printf("%d failures\n", fails);
		return 1;
	}
	printf("cdp1802: all tests passed\n");
	return 0;
}
