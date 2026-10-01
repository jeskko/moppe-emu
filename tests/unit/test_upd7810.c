/*
 * uPD7810 core unit test: small hand-assembled programs (encodings from
 * the uPD78C10A data sheet, cross-checked with as7810), expected values
 * worked out by hand.  A program ends at HLT with every interrupt masked.
 */
#include <stdio.h>
#include <string.h>

#include "../../upd7810.h"

static uint8_t mem[65536];
static int fails;
static int loopback;		/* TxD wired to RxD */
static upd7810 c;

static uint8_t rd(void *x, uint16_t a) { (void)x; return mem[a]; }
static void wr(void *x, uint16_t a, uint8_t v) { (void)x; mem[a] = v; }
static uint8_t adc(void *x, int ch) { (void)x; return (uint8_t)(ch * 10 + 1); }
static void txd(void *x, int l) { (void)x; if (loopback) upd7810_set_rxd(&c, l); }

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

/* load program at 0, reset, run until a halt that nothing can end */
static uint64_t
run(const uint8_t *prog, size_t n, const uint8_t *data, size_t dn, uint16_t daddr)
{
	upd7810_bus b = { 0 };
	memset(mem, 0, sizeof(mem));
	memcpy(mem, prog, n);
	if (data)
		memcpy(mem + daddr, data, dn);
	b.read = rd; b.write = wr; b.adc = adc; b.txd = txd;
	upd7810_init(&c, &b);
	for (int i = 0; i < 200000; i++) {
		upd7810_step(&c);
		if (c.halt && c.mkl == 0xFF && (c.mkh & 7) == 7)
			break;
	}
	return c.states;
}

#define RUN(...) do { static const uint8_t p_[] = { __VA_ARGS__ }; run(p_, sizeof(p_), NULL, 0, 0); } while (0)
#define HLT 0x48, 0x3B

int
main(void)
{
	uint64_t t;

	/* MVI A,80; ADI A,90 -> 10, CY */
	RUN(0x69, 0x80, 0x46, 0x90, HLT);
	CHECK(c.a == 0x10 && (c.psw & UPD_CY) && !(c.psw & UPD_Z));
	/* SUI A,1 from 0 -> FF CY; ACI A,0 -> 00 Z (the firmware's saturating decrement) */
	RUN(0x69, 0x00, 0x66, 0x01, 0x56, 0x00, HLT);
	CHECK(c.a == 0x00 && (c.psw & UPD_Z) && (c.psw & UPD_CY));
	/* ADI A,1 from FF; SBI A,0 -> FF (saturating increment) */
	RUN(0x69, 0xFF, 0x46, 0x01, 0x76, 0x00, HLT);
	CHECK(c.a == 0xFF);

	/* skip: MVI A,5; GTI A,4 (skips); MVI B,1; MVI C,2 */
	RUN(0x69, 0x05, 0x27, 0x04, 0x6A, 0x01, 0x6B, 0x02, HLT);
	CHECK(c.b == 0 && c.c == 2);
	/* GTI A,5 does not skip (5 - 5 - 1 borrows) */
	RUN(0x69, 0x05, 0x27, 0x05, 0x6A, 0x01, 0x6B, 0x02, HLT);
	CHECK(c.b == 1 && c.c == 2);
	/* LTI A,6 skips; NEI A,5 does not; EQI A,5 skips */
	RUN(0x69, 0x05, 0x37, 0x06, 0x6A, 0x01, 0x67, 0x05, 0x6B, 0x02,
	    0x77, 0x05, 0x6C, 0x03, HLT);
	CHECK(c.b == 0 && c.c == 2 && c.d == 0);
	/* ONI A,4 skips for A=5; OFFI A,2 skips */
	RUN(0x69, 0x05, 0x47, 0x04, 0x6A, 0x01, 0x57, 0x02, 0x6B, 0x02, HLT);
	CHECK(c.b == 0 && c.c == 0);
	/* skipped instruction states: NOP 4, GTI 7, skipped MVI 7, HLT 12 */
	t = run((const uint8_t[]){ 0x00, 0x27, 0x00, 0x6A, 0x01, HLT }, 7, NULL, 0, 0);
	CHECK(t == 4 + 7 + 7 + 12);
	/* SLLC A skips on carry: MVI A,81; SLLC A; MVI B,1 -> A 02, B 0 */
	RUN(0x69, 0x81, 0x48, 0x05, 0x6A, 0x01, HLT);
	CHECK(c.a == 0x02 && c.b == 0);
	/* INR A from FF skips; DCR B from 0 skips */
	RUN(0x69, 0xFF, 0x41, 0x6B, 0x01, 0x52, 0x6C, 0x01, HLT);
	CHECK(c.a == 0 && c.b == 0xFF && c.c == 0 && c.d == 0);

	/* string effect: MVI A,1; MVI A,2 -> 1; NOP; MVI A,3 -> 3 */
	RUN(0x69, 0x01, 0x69, 0x02, HLT);
	CHECK(c.a == 1);
	RUN(0x69, 0x01, 0x69, 0x02, 0x00, 0x69, 0x03, HLT);
	CHECK(c.a == 3);
	/* LXI HL twice: the second is skipped */
	RUN(0x34, 0x34, 0x12, 0x34, 0x78, 0x56, HLT);
	CHECK(c.h == 0x12 && c.l == 0x34);

	/* MUL / DIV: 200 * 100 = 20000; / 7 = 2857 r 1 */
	RUN(0x69, 200, 0x6A, 100, 0x48, 0x2E, 0x6B, 7, 0x48, 0x3F, HLT);
	CHECK(c.ea == 2857 && c.c == 1);
	/* DIV A: 1234 / 10 = 123 r 4 (format_EA) */
	RUN(0x44, 0xD2, 0x04, 0x69, 10, 0x48, 0x3D, HLT);
	CHECK(c.ea == 123 && c.a == 4);

	/* CALL / RETS: the returned-to MVI B is skipped */
	{
		static uint8_t p[0x110] = { 0x04, 0x00, 0x80, 0x40, 0x00, 0x01,
		    0x6A, 0x01, 0x6B, 0x02, HLT };
		p[0x100] = 0xB9;
		run(p, sizeof(p), NULL, 0, 0);
		CHECK(c.b == 0 && c.c == 2 && c.sp == 0x8000);
	}
	/* PUSH VA; POP BC */
	RUN(0x04, 0x00, 0x80, 0x68, 0x12, 0x69, 0x34, 0xB0, 0xA1, HLT);
	CHECK(c.b == 0x12 && c.c == 0x34 && mem[0x7FFE] == 0x34 && mem[0x7FFF] == 0x12);
	/* EXA / EXX */
	RUN(0x69, 0x11, 0x6A, 0x22, 0x10, 0x11, 0x69, 0x33, 0x6A, 0x44, 0x10, 0x11, HLT);
	CHECK(c.a == 0x11 && c.b == 0x22 && c.a2 == 0x33 && c.b2 == 0x44);

	/* LDAX [HL+A] */
	{
		static const uint8_t p[] = { 0x34, 0x00, 0x02, 0x69, 0x05, 0xAC, HLT };
		static const uint8_t d[] = { 0x77 };
		run(p, sizeof(p), d, 1, 0x205);
		CHECK(c.a == 0x77);
	}
	/* LDAX [DE+byte]; STAX [HL]+ */
	{
		static const uint8_t p[] = { 0x24, 0x00, 0x02, 0xAB, 0x03, 0x34, 0x00, 0x03, 0x3D, HLT };
		static const uint8_t d[] = { 0, 0, 0, 0x66 };
		run(p, sizeof(p), d, 4, 0x200);
		CHECK(c.a == 0x66 && mem[0x300] == 0x66 && c.h == 0x03 && c.l == 0x01);
	}
	/* STEAX [DE++]; LDEAX [HL+byte] */
	RUN(0x44, 0xEF, 0xBE, 0x24, 0x00, 0x03, 0x48, 0x94, 0x34, 0xFE, 0x02,
	    0x44, 0, 0, 0x48, 0x8F, 0x02, HLT);
	CHECK(mem[0x300] == 0xEF && mem[0x301] == 0xBE && c.d == 0x03 && c.e == 0x02 && c.ea == 0xBEEF);
	/* TABLE at 2 with A = 1 reads 6, 7 */
	RUN(0x69, 0x01, 0x48, 0xA8, HLT, 0x11, 0x22);
	CHECK(c.c == 0x11 && c.b == 0x22);
	/* BLOCK: 3 bytes 400 -> 500 */
	{
		static const uint8_t p[] = { 0x34, 0x00, 0x04, 0x24, 0x00, 0x05, 0x6B, 0x02, 0x31, HLT };
		static const uint8_t d[] = { 1, 2, 3, 4 };
		run(p, sizeof(p), d, 4, 0x400);
		CHECK(mem[0x500] == 1 && mem[0x502] == 3 && mem[0x503] == 0 && c.c == 0xFF && c.l == 0x03);
	}
	/* CALT 0 via 0x0080 -> 0x0120; CALF 0x0810 */
	{
		static uint8_t p[0x900] = { 0x04, 0x00, 0x80, 0x80 };
		p[0x80] = 0x20; p[0x81] = 0x01;
		p[0x120] = 0x78; p[0x121] = 0x10;
		p[0x810] = 0x69; p[0x811] = 0x42; p[0x812] = 0x48; p[0x813] = 0x3B;
		run(p, sizeof(p), NULL, 0, 0);
		CHECK(c.a == 0x42 && c.sp == 0x8000 - 4 && mem[0x7FFE] == 0x04 && mem[0x7FFC] == 0x22);
	}
	/* MVI B,2; 2: DCR B; JRE 2 (skipped once DCR borrows) */
	RUN(0x6A, 0x02, 0x52, 0x4F, 0xFD, HLT);
	CHECK(c.b == 0xFF);
	/* DAA: 09 + 01 = 0A -> 10 */
	RUN(0x69, 0x09, 0x46, 0x01, 0x61, HLT);
	CHECK(c.a == 0x10);
	/* 16-bit: EA = 1000; DGT EA, HL with HL=999 skips */
	RUN(0x44, 0xE8, 0x03, 0x34, 0xE7, 0x03, 0x74, 0xAF, 0x6A, 0x01, HLT);
	CHECK(c.b == 0);
	/* DEQ EA, HL equal skips; DADD */
	RUN(0x44, 0x34, 0x12, 0x34, 0x34, 0x12, 0x74, 0xFF, 0x6A, 0x01, 0x74, 0xC7, HLT);
	CHECK(c.b == 0 && c.ea == 0x2468);
	/* MOV r,word / word,r and xxW: V=02; MVIW 10,7; ADDW 10 */
	RUN(0x68, 0x02, 0x71, 0x10, 0x07, 0x69, 0x03, 0x74, 0xC0, 0x10, 0x70, 0x79, 0x00, 0x03, HLT);
	CHECK(c.a == 10 && mem[0x0300] == 10);
	/* NEIW: MVIW 10,5; NEIW 10,5 does not skip; EQIW skips */
	RUN(0x68, 0x02, 0x71, 0x10, 0x05, 0x65, 0x10, 0x05, 0x6A, 0x01, 0x75, 0x10, 0x05, 0x6B, 0x01, HLT);
	CHECK(c.b == 1 && c.c == 0);
	/* ORI PA, 0x40 (special register read-modify-write, PA all output) */
	RUN(0x69, 0x00, 0x4D, 0xD2, 0x64, 0x00, 0x05, 0x64, 0x18, 0x40, HLT);
	CHECK(c.port[UPD_PA] == 0x45);

	/* timer 1 interrupt: TM1 = 10 at f/12 -> INTFT1 after 40 states */
	{
		uint8_t p[0x40] = { 0 };
		/* 0: LXI SP; JMP 0x30 */
		p[0] = 0x04; p[1] = 0x00; p[2] = 0x80; p[3] = 0x54; p[4] = 0x30; p[5] = 0x00;
		/* 8: ISR: MVI B,55; MVI MKL,FF; HLT */
		p[8] = 0x6A; p[9] = 0x55; p[10] = 0x64; p[11] = 0x07; p[12] = 0xFF; p[13] = 0x48; p[14] = 0x3B;
		/* 30: TM1 = 10; TMM = 0x0F: T1 f/12; ANI MKL,~04; EI; JR . */
		const uint8_t m[] = { 0x69, 10, 0x4D, 0xDB, 0x69, 0x0F, 0x4D, 0xCD,
		    0x64, 0x0F, 0xFB, 0xAA, 0xFF };
		memcpy(p + 0x30, m, sizeof(m));
		t = run(p, sizeof(p), NULL, 0, 0);
		CHECK(c.b == 0x55 && c.sp == 0x8000 - 3 && mem[0x7FFD] == 0x3C && c.ie == 0);
	}
	/* EI delay: request pending, EI; MVI C,1 runs before the interrupt */
	{
		uint8_t p[0x60] = { 0 };
		p[0] = 0x04; p[1] = 0x00; p[2] = 0x80; p[3] = 0x54; p[4] = 0x30; p[5] = 0x00;
		p[8] = 0x0B; p[9] = 0x1A; p[10] = 0x64; p[11] = 0x07; p[12] = 0xFF; p[13] = 0x48; p[14] = 0x3B;	/* MOV A,C; MOV B,A */
		const uint8_t m[] = { 0x69, 2, 0x4D, 0xDB, 0x69, 0x0F, 0x4D, 0xCD,
		    0x00, 0x00, 0x00, 0x00, 0x64, 0x0F, 0xFB, 0xAA, 0x6B, 0x01, 0xFF };
		memcpy(p + 0x30, m, sizeof(m));
		run(p, sizeof(p), NULL, 0, 0);
		CHECK(c.b == 1);
	}

	/* serial loopback, 9600 8O1 from TO (TM0 = 3 at f/12), TXB A5 -> RXB */
	loopback = 1;
	{
		static const uint8_t p[] = {
			0x69, 0x03, 0x4D, 0xDA,		/* TM0 = 3 */
			0x69, 0x60, 0x4D, 0xCD,		/* TMM: T0 f/12 into F/F, T1 stop */
			0x69, 0x03, 0x4D, 0xD1,		/* MCC: TxD, RxD */
			0x69, 0x5E, 0x4D, 0xCA,		/* SML */
			0x69, 0x0C, 0x4D, 0xC9,		/* SMH */
			0x69, 0xA5, 0x4D, 0xD8,		/* TXB */
			0x48, 0x49, 0xFD,		/* SKIT INTFSR; JR -3 */
			0x4C, 0xD9,			/* MOV A, RXB */
			HLT };
		t = run(p, sizeof(p), NULL, 0, 0);
		CHECK(c.a == 0xA5 && !(c.irr & (1u << UPD_ER)));
		/* received at the middle of the stop bit: 10.5 bits at 9600 bd = 4032 states */
		CHECK(t > 4032 && t < 4032 + 300);
	}
	loopback = 0;

	/* timer/event counter: ETM0 = 100, f/12 -> INTFE0 at ECNT 100 */
	{
		static const uint8_t p[] = {
			0x44, 100, 0, 0x48, 0xD2,	/* DMOV ETM0, EA */
			0x48, 0x45, 0xFD,		/* SKIT INTFE0; JR -3 */
			0x48, 0xC0, HLT };		/* DMOV EA, ECNT */
		t = run(p, sizeof(p), NULL, 0, 0);
		CHECK(c.ea >= 100 && c.ea < 110 && t > 400 && t < 450);
	}
	/* A/D scan: CR2 = AN2 after 4 conversions; ANM 08 -> AN6 */
	{
		static const uint8_t p[] = {
			0x48, 0x48, 0xFD,		/* SKIT INTFAD; JR -3 */
			0x4C, 0xE2,			/* MOV A, CR2 */
			0x1A,				/* MOV B, A */
			0x69, 0x08, 0x4D, 0xC8,		/* ANM = 08 */
			0x48, 0x48, 0xFD,		/* SKIT INTFAD; JR -3 */
			0x4C, 0xE2, HLT };
		run(p, sizeof(p), NULL, 0, 0);
		CHECK(c.b == 21 && c.a == 61);
	}
	/* HLT ends on an unmasked request even with IE = 0; SKIT AN5 */
	{
		static const uint8_t p[] = {
			0x69, 0x10, 0x4D, 0xDB, 0x69, 0x0F, 0x4D, 0xCD,	/* TM1 16, f/12 */
			0x64, 0x0F, 0xFB,		/* unmask FT1 */
			0x48, 0x3B,			/* HLT (released by FT1) */
			0x64, 0x07, 0xFF,		/* mask all */
			0x48, 0x51, 0x6A, 0x01,		/* SKIT AN5; MVI B,1 */
			HLT };
		upd7810_bus b = { 0 };
		memset(mem, 0, sizeof(mem));
		memcpy(mem, p, sizeof(p));
		b.read = rd; b.write = wr;
		upd7810_init(&c, &b);
		for (int i = 0; i < 1000 && c.pc < 13; i++)
			upd7810_step(&c);
		upd7810_an_edge(&c, 1);
		for (int i = 0; i < 1000; i++) {
			upd7810_step(&c);
			if (c.halt && c.mkl == 0xFF)
				break;
		}
		CHECK(c.b == 0 && c.pc == 22 && !(c.irr & (1u << UPD_AN5)));
	}

	if (fails)
		printf("%d failures\n", fails);
	else
		printf("upd7810: all tests passed\n");
	return fails != 0;
}
