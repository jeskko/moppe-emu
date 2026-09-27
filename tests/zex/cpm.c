/* Minimal CP/M environment to run zexdoc/zexall against the Z80 core. */
#include <stdio.h>
#include <stdlib.h>
#include "../../z80.h"

static uint8_t mem[65536];
static uint8_t rd(void *c, uint16_t a) { (void)c; return mem[a]; }
static void wr(void *c, uint16_t a, uint8_t v) { (void)c; mem[a] = v; }
static uint8_t in(void *c, uint16_t p) { (void)c; (void)p; return 0xff; }
static void out(void *c, uint16_t p, uint8_t v) { (void)c; (void)p; (void)v; }

int
main(int argc, char **argv)
{
	FILE *fp = fopen(argv[1], "rb");
	if (!fp) { perror(argv[1]); return 2; }
	fread(mem + 0x100, 1, 0xff00, fp);
	fclose(fp);
	mem[0] = 0x76;			/* warm boot -> HALT */
	mem[5] = 0xc9;			/* BDOS entry: RET, trapped below */
	mem[6] = 0x00; mem[7] = 0xf0;	/* top of TPA for SP setup */

	z80 z = { .read = rd, .write = wr, .in = in, .out = out };
	z80_reset(&z);
	z.pc = 0x100;
	z.sp = 0xf000;
	unsigned long long t = 0;
	for (;;) {
		if (z.pc == 5) {
			if (z.c == 2)
				putchar(z.e);
			else if (z.c == 9)
				for (uint16_t a = (z.d << 8) | z.e; mem[a] != '$'; a++)
					putchar(mem[a]);
			fflush(stdout);
		}
		t += z80_step(&z);
		if (z.halted)
			break;
	}
	printf("\n[%llu T-states]\n", t);
	return 0;
}
