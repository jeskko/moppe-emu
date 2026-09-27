#include <stdio.h>
#include <assert.h>
#include "../../pit.h"

static unsigned long now, edges[64][2]; static int nedge;
static void cb(void *c, int n, int lv, unsigned off) { (void)c; (void)n; if (nedge < 64) { edges[nedge][0] = now + off; edges[nedge][1] = lv; nedge++; } }

static void run(pit *p, int n, unsigned clocks, unsigned step)
{
	for (unsigned i = 0; i < clocks; i += step) { pit_clock(p, n, step); now += step; }
}

int main(void)
{
	pit p = { .out_changed = cb };
	int fail = 0;

	/* mode 3, N=10: 5 high 5 low, period 10 */
	pit_init(&p); now = 0; nedge = 0;
	pit_write(&p, 3, 0x40 | 0x30 | 0x06); pit_write(&p, 1, 10); pit_write(&p, 1, 0);
	run(&p, 1, 100, 3);
	for (int i = 1; i + 1 < nedge; i++) {
		unsigned long d = edges[i + 1][0] - edges[i][0];
		if (d != 5) { printf("mode3 even: edge %d delta %lu\n", i, d); fail = 1; break; }
	}
	/* mode 3, N=7: high 4, low 3 */
	pit_init(&p); now = 0; nedge = 0;
	pit_write(&p, 3, 0x40 | 0x30 | 0x06); pit_write(&p, 1, 7); pit_write(&p, 1, 0);
	run(&p, 1, 100, 1);
	for (int i = 1; i + 1 < nedge; i++) {
		unsigned long d = edges[i + 1][0] - edges[i][0];
		unsigned want = edges[i][1] ? 4 : 3;
		if (d != want) { printf("mode3 odd: edge %d lv %lu delta %lu want %u\n", i, edges[i][1], d, want); fail = 1; break; }
	}
	/* mode 2, N=5: period 5, low 1 clock */
	pit_init(&p); now = 0; nedge = 0;
	pit_write(&p, 3, 0x40 | 0x30 | 0x04); pit_write(&p, 1, 5); pit_write(&p, 1, 0);
	run(&p, 1, 60, 7);
	for (int i = 0; i + 2 < nedge; i += 2) {
		if (edges[i][1] != 0 || edges[i + 1][0] - edges[i][0] != 1 || edges[i + 2][0] - edges[i][0] != 5) {
			printf("mode2: bad at %d\n", i); fail = 1; break; }
	}
	/* mode 0 LSB, 65 counts, the check_for_P8E_cpu trick */
	pit_init(&p);
	pit_write(&p, 3, 0x40 | 0x10 | 0x00); pit_write(&p, 1, 65);
	pit_clock(&p, 1, 50);
	uint8_t e = pit_read(&p, 1);
	pit_init(&p);
	pit_write(&p, 3, 0x40 | 0x10 | 0x00); pit_write(&p, 1, 65);
	pit_clock(&p, 1, 90);
	uint8_t n = pit_read(&p, 1);
	if ((e & 0x80) || !(n & 0x80)) { printf("mode0 P8E check: e=%02x n=%02x\n", e, n); fail = 1; }
	printf(fail ? "pit: FAIL\n" : "pit: ok\n");
	return fail;
}
