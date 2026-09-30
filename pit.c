#include "pit.h"

#include <string.h>

void
pit_init(pit *p)
{
	void (*cb)(void *, int, int, unsigned) = p->out_changed;
	void *ctx = p->ctx;
	unsigned quiet = p->quiet;

	memset(p, 0, sizeof(*p));
	p->out_changed = cb;
	p->ctx = ctx;
	p->quiet = quiet;
	for (int i = 0; i < 3; i++) {
		p->c[i].out = 1;	/* undefined at power-up; high is typical */
		p->c[i].null = 1;
	}
}

static void
set_out(pit *p, int n, int level, unsigned off)
{
	pit_counter *c = &p->c[n];

	if (c->out == level)
		return;
	c->out = level;
	if (p->out_changed && !(p->quiet & (1u << n)))
		p->out_changed(p->ctx, n, level, off);
}

static uint32_t
eff_count(const pit_counter *c)
{
	return c->cr ? c->cr : 0x10000;	/* binary mode; BCD not used */
}

static void
count_written(pit *p, int n)
{
	pit_counter *c = &p->c[n];
	int first = c->null;

	c->null = 0;
	switch (c->mode) {
	case 0:
		set_out(p, n, 0, 0);
		c->load = 1;
		c->armed = 1;
		break;
	case 4:
		c->load = 1;
		c->armed = 1;
		break;
	case 2:
	case 3:
		/* first count after a mode set starts counting on the next
		 * clock; later writes take effect at the next reload */
		if (first)
			c->load = 1;
		break;
	default:	/* modes 1, 5: need a gate trigger, never comes */
		break;
	}
}

void
pit_write(pit *p, int addr, uint8_t v)
{
	if (addr == 3) {
		int sc = v >> 6;
		if (sc == 3) {				/* read-back */
			for (int n = 0; n < 3; n++) {
				if (!(v & (2 << n)))
					continue;
				pit_counter *c = &p->c[n];
				if (!(v & 0x20) && !c->latched) {
					c->latched = 1;
					c->latch = c->ce;
					c->rlatched = 0;
				}
				if (!(v & 0x10) && !c->status_latched) {
					c->status_latched = 1;
					c->status = (c->out << 7) | (c->null << 6)
					          | (c->rw << 4) | (c->mode << 1) | c->bcd;
				}
			}
			return;
		}
		pit_counter *c = &p->c[sc];
		int rw = (v >> 4) & 3;
		if (rw == 0) {				/* counter latch */
			if (!c->latched) {
				c->latched = 1;
				c->latch = c->ce;
				c->rlatched = 0;
			}
			return;
		}
		c->rw = rw;
		c->mode = (v >> 1) & 7;
		if (c->mode > 5)
			c->mode -= 4;			/* 6,7 alias 2,3 */
		c->bcd = v & 1;
		c->null = 1;
		c->load = 0;
		c->armed = 0;
		c->wlsb = c->rmsb = 0;
		c->latched = 0;
		c->status_latched = 0;
		c->sq_left = 0;
		c->pending_high = 0;
		c->ce = 0;
		set_out(p, sc, c->mode == 0 ? 0 : 1, 0);
		return;
	}

	pit_counter *c = &p->c[addr];
	switch (c->rw) {
	case 1:
		c->cr = v;
		count_written(p, addr);
		break;
	case 2:
		c->cr = v << 8;
		count_written(p, addr);
		break;
	case 3:
		if (!c->wlsb) {
			c->cr = (c->cr & 0xff00) | v;
			c->wlsb = 1;
			if (c->mode == 0) {
				c->armed = 0;		/* counting stops meanwhile */
				c->load = 0;
			}
		} else {
			c->cr = (c->cr & 0x00ff) | (v << 8);
			c->wlsb = 0;
			count_written(p, addr);
		}
		break;
	}
}

uint8_t
pit_read(pit *p, int addr)
{
	if (addr == 3)
		return 0xff;		/* no read at control address */
	pit_counter *c = &p->c[addr];

	if (c->status_latched) {
		c->status_latched = 0;
		return c->status;
	}
	uint16_t v = c->latched ? c->latch : c->ce;
	uint8_t r;
	switch (c->rw) {
	case 1:
		r = v;
		c->latched = 0;
		break;
	case 2:
		r = v >> 8;
		c->latched = 0;
		break;
	default:
		if (!c->rmsb) {
			r = v;
			c->rmsb = 1;
		} else {
			r = v >> 8;
			c->rmsb = 0;
			c->latched = 0;
		}
		break;
	}
	return r;
}

/*
 * Advance counter n by nclocks input clocks, in event-sized strides.
 * Mode 3 keeps the remaining clocks of the current half period in
 * sq_left; CE reads back as twice that (close to the real chip).
 */
void
pit_clock(pit *p, int n, unsigned nclocks)
{
	pit_counter *c = &p->c[n];
	unsigned off = 0;

	while (off < nclocks) {
		unsigned left = nclocks - off;
		uint32_t cur;

		if (c->null)
			return;
		if (c->pending_high) {
			c->pending_high = 0;
			set_out(p, n, 1, off);
		}
		if (c->load) {
			c->load = 0;
			off++;
			if (c->mode == 3) {
				uint32_t cnt = eff_count(c);
				set_out(p, n, 1, off - 1);
				c->sq_left = (cnt + 1) / 2;
				c->ce = (c->sq_left * 2) & 0xffff;
			} else
				c->ce = c->cr;
			continue;
		}
		switch (c->mode) {
		case 0:
		case 4:
			cur = c->ce ? c->ce : 0x10000;
			if (!c->armed || cur > left) {
				c->ce -= left;
				return;
			}
			off += cur;
			c->ce = 0;
			c->armed = 0;
			if (c->mode == 0)
				set_out(p, n, 1, off - 1);
			else {
				set_out(p, n, 0, off - 1);
				if (off < nclocks)
					set_out(p, n, 1, off);
				else
					c->pending_high = 1;
			}
			continue;
		case 2:
			cur = c->ce ? c->ce : 0x10000;
			/* unwatched: whole periods (from a CE within one; a CE
			 * from an older count first counts down normally) */
			if ((p->quiet & (1u << n)) && c->cr != 1) {
				uint32_t per = eff_count(c);
				if (cur <= per && left > per) {
					off += (left - 1) / per * per;
					left = nclocks - off;
				}
			}
			if (cur > 1) {
				uint32_t k = cur - 1;		/* clocks until CE == 1 */
				if (k > left) {
					c->ce = cur - left;
					return;
				}
				off += k;
				c->ce = 1;
				set_out(p, n, 0, off - 1);
				continue;
			}
			/* CE == 1: next clock reloads and OUT returns high */
			off++;
			c->ce = c->cr;
			set_out(p, n, 1, off - 1);
			continue;
		case 3:
			/* unwatched: whole periods, when the half in progress is
			 * no longer than the current count's (a new count takes
			 * effect at the next half) */
			if ((p->quiet & (1u << n)) && c->cr != 1) {
				uint32_t per = eff_count(c);
				if (c->sq_left <= (c->out ? (per + 1) / 2 : per / 2) && left > per) {
					off += (left - 1) / per * per;
					left = nclocks - off;
				}
			}
			if (c->sq_left > left) {
				c->sq_left -= left;
				c->ce = (c->sq_left * 2) & 0xffff;
				return;
			}
			off += c->sq_left;
			{
				uint32_t cnt = eff_count(c);
				int nl = !c->out;
				set_out(p, n, nl, off - 1);
				c->sq_left = nl ? (cnt + 1) / 2 : cnt / 2;
				if (c->sq_left == 0)
					c->sq_left = 1;
			}
			continue;
		default:
			return;
		}
	}
}
