#include "cu53an.h"

#include <string.h>

#define SEL_KEYPAD 0
#define SEL_LATCH  1
#define SEL_LCD1   2
#define SEL_LCD2   3

void
cu53an_init(cu53an *c)
{
	memset(c, 0, sizeof(*c));
	c->key = CU53_KEY_NONE;
	c->sel = SEL_KEYPAD;
	c->bit1 = 1;
}

static void
lcd_deselect(cu53an *c, int chip)
{
	/* PCF2111: exactly 0, 32 data bits, load bit */
	if (c->rxn == 34 && !(c->rx & 1)) {
		uint32_t data = (uint32_t)(c->rx >> 1);
		int half = (c->rx >> 33) & 1;
		c->lcd[chip][half] = data;
		c->frames++;
	}
	c->rxn = 0;
	c->rx = 0;
}

static uint8_t
parallel_word(const cu53an *c)
{
	/* encoder outputs are enabled only while DA is high; otherwise the
	 * inputs float/pull high */
	uint8_t code = c->key >= 0 ? (uint8_t)c->key : 0x1f;
	return (uint8_t)((c->ldr_dark ? 0x80 : 0) | ((code & 0x1f) << 2) |
	                 (c->bit1 ? 0x02 : 0) | 0x01);
}

void
cu53an_out2(cu53an *c, uint8_t out2)
{
	uint8_t sel = (out2 >> 4) & 3;
	uint8_t clk = (out2 >> 6) & 1;
	uint8_t dp  = (out2 >> 7) & 1;

	if (sel != c->sel) {
		if (c->sel == SEL_LCD1 || c->sel == SEL_LCD2)
			lcd_deselect(c, c->sel - SEL_LCD1);
		c->sel = sel;
		if (sel == SEL_LATCH)
			c->latch = c->shift;
	}
	if (clk && !c->clk) {
		/* rising edge */
		if (sel == SEL_KEYPAD) {
			c->shift = parallel_word(c);
		} else {
			/* bits enter at bit 0 and move toward bit 7 (DCU) */
			c->shift = (uint8_t)((c->shift << 1) | dp);
		}
		if (sel == SEL_LCD1 || sel == SEL_LCD2) {
			if (c->rxn < 64)
				c->rx |= (uint64_t)dp << c->rxn;
			c->rxn++;
		}
	}
	c->clk = clk;
	if (c->sel == SEL_LATCH)
		c->latch = c->shift;
}

int
cu53an_dcu(const cu53an *c)
{
	return (c->shift >> 7) & 1;
}

int
cu53an_da(const cu53an *c)
{
	return c->key >= 0;
}

void
cu53an_set_key(cu53an *c, int code)
{
	c->key = code;
}

int
cu53an_segment(const cu53an *c, int pos)
{
	int half = (pos >> 6) & 1, chip = (pos >> 5) & 1, j = pos & 31;
	return (c->lcd[chip][half] >> j) & 1;
}
