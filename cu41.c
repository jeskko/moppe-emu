/*
 * CU41 control unit.  See cu41.h.
 */
#include "cu41.h"

#include <string.h>

void
cu41_init(cu41 *c)
{
	memset(c, 0, sizeof(*c));
	memset(c->cell, ' ', sizeof(c->cell));
	c->key = -1;
	c->keyclk = 1;
}

static void
dpy_byte(cu41 *c, uint8_t b)
{
	c->bytes++;
	c->last_byte = b;
	if (b >= 0x80) {
		c->commands++;
		c->last_command = b;
		c->pos = 0;
	} else if (b == CU41_DP) {
		c->dp |= (uint16_t)(1u << ((c->pos + CU41_CELLS - 1) % CU41_CELLS));
	} else {
		c->cell[c->pos] = b;
		c->dp &= (uint16_t)~(1u << c->pos);
		c->pos = (c->pos + 1) % CU41_CELLS;
	}
}

void
cu41_out1(cu41 *c, uint8_t v)
{
	uint8_t sdo = v & 1, keyclk = (v >> 1) & 1, dpyclk = (v >> 2) & 1;
	uint8_t rst = !((v >> 3) & 1);

	if (rst) {
		if (!c->rst) {
			memset(c->cell, ' ', sizeof(c->cell));
			c->dp = 0;
			c->pos = 0;
		}
		c->nbits = 0;
		c->rx = 0;
	} else if (dpyclk && !c->dpyclk) {
		c->rx = (uint8_t)((c->rx << 1) | sdo);
		if (++c->nbits == 8) {
			dpy_byte(c, c->rx);
			c->nbits = 0;
			c->rx = 0;
		}
	}
	c->rst = rst;
	c->dpyclk = dpyclk;

	/* CLK1 trailing edge shifts the key register while DA is low */
	if (!keyclk && c->keyclk && c->key < 0) {
		if (c->nout > 0) {
			c->sdi = c->out & 1;
			c->out >>= 1;
			c->nout--;
		} else {
			c->sdi = 0;
		}
	}
	c->keyclk = keyclk;
}

int
cu41_sdi(const cu41 *c)
{
	return c->sdi;
}

void
cu41_key(cu41 *c, int code)
{
	if (code >= 0) {
		c->key = code;
		c->sdi = 1;		/* DA: Q8 high while held */
	} else if (c->key >= 0) {
		/* P/S = 0: start bit 0, then the code LSB first */
		c->out = (uint16_t)((c->key & 0xff) << 1);
		c->nout = 9;
		c->key = -1;
	}
}
