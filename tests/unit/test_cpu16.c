/*
 * CPU16 core unit test: small programs assembled by the hc16 GNU assembler
 * (test_cpu16_progs.h), expected values worked out by hand from the CPU16
 * Reference Manual.  A program starts at 0x0200 in bank 0 (reset vector:
 * ZK = SK = PK = 0, SP = 7FFE), stores its results from 0x1000 on and
 * ends at WAI.
 */
#include <stdio.h>
#include <string.h>

#include "../../cpu16.h"
#include "test_cpu16_progs.h"

static uint8_t mem[0x100000];
static int fails;
static cpu16 c;
static int iack_vec = 0x40;

static uint8_t rd8(void *x, uint32_t a) { (void)x; return mem[a]; }
static void wr8(void *x, uint32_t a, uint8_t v) { (void)x; mem[a] = v; }
static uint16_t rd16(void *x, uint32_t a) { (void)x; return (uint16_t)(mem[a] << 8 | mem[a + 1]); }
static void wr16(void *x, uint32_t a, uint16_t v) { (void)x; mem[a] = (uint8_t)(v >> 8); mem[a + 1] = (uint8_t)v; }
static int clk(void *x, uint32_t a, int size) { (void)x; (void)a; (void)size; return 2; }
static int iack(void *x, int level) { (void)x; (void)level; c.irq_level = 0; return iack_vec; }

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

static uint16_t w(uint32_t a) { return (uint16_t)(mem[a] << 8 | mem[a + 1]); }
static void setw(uint32_t a, uint16_t v) { mem[a] = (uint8_t)(v >> 8); mem[a + 1] = (uint8_t)v; }

static void
load(const uint8_t *prog, size_t n)
{
	cpu16_bus b = { 0, rd8, wr8, rd16, wr16, clk, iack };
	memset(mem, 0, sizeof(mem));
	setw(0, 0x0000);		/* ZK SK PK */
	setw(2, 0x0200);		/* PC */
	setw(4, 0x7FFE);		/* SP */
	setw(6, 0x0000);		/* IZ */
	memcpy(mem + 0x200, prog, n);
	cpu16_init(&c, &b);
}

static void
run(void)
{
	cpu16_reset(&c);
	for (int i = 0; i < 100000 && !c.waiting; i++)
		cpu16_step(&c);
	CHECK(c.waiting);
	CHECK(!c.illegal);
}

static void
test_alu(void)
{
	load(prog_alu, sizeof(prog_alu));
	run();
	CHECK(w(0x1000) == 0x8AE0);	/* S N V, IP 7 */
	CHECK(mem[0x1002] == 0x81);	/* F0 + 20: C */
	CHECK(mem[0x1003] == 0x89);	/* 5 - 6: N C */
	CHECK(mem[0x1004] == 0x47);	/* 19 + 28, DAA */
	CHECK(mem[0x1005] == 0x00);	/* 99 + 1, DAA */
	CHECK(mem[0x1006] == 0x85);	/* ... Z C */
	CHECK(w(0x1008) == 0x4000);	/* 8000 ASRD = C000, NEGW */
	CHECK(w(0x100A) == 0xFFFF);	/* NEGE 1 */
	CHECK(w(0x100C) == 0xFF80);	/* SXT */
}

static void
test_branch(void)
{
	load(prog_branch, sizeof(prog_branch));
	run();
	CHECK(mem[0x1000] == 7);	/* BSR, JSR, LBSR, RTS */
	CHECK(mem[0x1001] == 0);
	CHECK(mem[0x1002] == 7);	/* LBRA skipped CLRA; BPL/BLS not taken, BHI taken */
	CHECK(c.sp == 0x7FFE);
}

static void
test_stack(void)
{
	load(prog_stack, sizeof(prog_stack));
	run();
	CHECK(w(0x1000) == 0x1111);
	CHECK(w(0x1002) == 0x2222);
	CHECK(w(0x1004) == 0x3333);
	CHECK(w(0x1006) == 0x4444);
	CHECK(w(0x1008) == 0x5555);
	CHECK(w(0x100A) == 0x8000);	/* TSX: SP + 2 */
	CHECK(mem[0x100C] == 0xAB);	/* PSHA/PULA */
	CHECK(w(0x100E) == 0x7FFE);
}

static void
test_div(void)
{
	load(prog_div, sizeof(prog_div));
	run();
	CHECK(w(0x1000) == 0x5555 && w(0x1002) == 0x0001);	/* EDIV 10000/3 */
	CHECK(w(0x1004) == 0xFFFB && w(0x1006) == 0xFFFF);	/* EDIVS -16/3 */
	CHECK(w(0x1008) == 0x2000 && w(0x100A) == 0x0000);	/* FMULS 0.5*0.5 */
	CHECK(w(0x100C) == 0x000E && w(0x100E) == 0x0002);	/* IDIV 100/7 */
	CHECK(w(0x1010) == 0x4000 && w(0x1012) == 0x0000);	/* FDIV 1000/4000 */
	CHECK(w(0x1014) == 0xFFFF && w(0x1016) == 0xFFFE);	/* EMULS -1*2 */
	CHECK(w(0x1018) == 0x009C);				/* MUL 12*13 */
}

static void
test_mac(void)
{
	load(prog_mac, sizeof(prog_mac));
	for (int i = 0; i < 4; i++) {
		setw(0x1100 + 2 * i, 0x4000);	/* 0.5 */
		setw(0x1200 + 2 * i, 0x2000);	/* 0.25 */
	}
	run();
	CHECK(w(0x1000) == 0x2000);	/* 2 x 0.125 */
	CHECK(w(0x1002) == 0x1104);
	CHECK(w(0x1004) == 0x1204);
	CHECK(w(0x1006) == 0x4000);	/* IZ = HR */
	CHECK(w(0x1008) == 0x1100);	/* modulo 4 on X */
	CHECK(w(0x100A) == 0x3000);	/* RMAC, E = 2: three iterations */
	CHECK(w(0x100C) == 0x1106);
}

static void
test_movbit(void)
{
	load(prog_movbit, sizeof(prog_movbit));
	mem[0x1100] = 0x12; mem[0x1101] = 0x34; mem[0x1102] = 0x56; mem[0x1103] = 0x78;
	run();
	CHECK(w(0x1000) == 0x1234);	/* MOVW IXP to EXT */
	CHECK(mem[0x1002] == 0x56);	/* MOVB after X += 2 */
	CHECK(w(0x1004) == 0x1103);
	CHECK(mem[0x1006] == 0x7E);	/* BSET/BCLR EXT */
	CHECK(mem[0x1008] == 0);	/* BRSET taken */
	CHECK(mem[0x1009] == 0);	/* BRCLR, BRSET IND8 taken */
	CHECK(mem[0x100A] == 0xC0);
	CHECK(w(0x100C) == 0x000E);	/* BCLRW */
}

static void
test_irq(void)
{
	load(prog_irq, sizeof(prog_irq));
	setw(0x000C, IRQ_SWIH);
	setw(0x0080, IRQ_IRQH);		/* vector 0x40 */
	c.irq_level = 3;		/* masked until ANDP lowers IP */
	run();
	CHECK(mem[0x1000] == 0x11);	/* SWI handler ran once, RTI to the next */
	CHECK(mem[0x1002] == 0x55);	/* interrupt handler */
	CHECK(mem[0x1001] == 0x11);
	CHECK(c.last_exc == 0x40);
	CHECK(c.sp == 0x7FFE);
}

static void
test_banks(void)
{
	load(prog_banks, sizeof(prog_banks));
	mem[0x20001] = 0x5A;
	mem[0x20010] = 0xA5;
	mem[0x10010] = 0x3C;
	run();
	CHECK(mem[0x1000] == 0x5A);	/* XK:IX + 3 crosses into bank 2 */
	CHECK(mem[0x1001] == 0xA5);	/* EK = 2 */
	CHECK(mem[0x1002] == 0x02);	/* AIX carries into XK */
	CHECK(w(0x1004) == 0x0002);
	CHECK(mem[0x1006] == 0x3C);	/* E,Y with YK = 1 */
}

int
main(void)
{
	test_alu();
	test_branch();
	test_stack();
	test_div();
	test_mac();
	test_movbit();
	test_irq();
	test_banks();
	if (fails) {
		printf("test_cpu16: %d failures\n", fails);
		return 1;
	}
	printf("test_cpu16: ok\n");
	return 0;
}
