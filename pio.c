#include "pio.h"

#include <string.h>

#define ICW_ENABLE 0x80
#define ICW_AND    0x40
#define ICW_HIGH   0x20

static uint8_t
pio_vector(void *ctx, int id)
{
	pio *p = ctx;
	return p->p[id].vector;
}

void
pio_init(pio *p)
{
	memset(p, 0, sizeof(*p));
	for (int i = 0; i < 2; i++) {
		p->p[i].irq.name = i ? "PIO B" : "PIO A";
		p->p[i].irq.vector = pio_vector;
		p->p[i].irq.ctx = p;
		p->p[i].irq.id = i;
	}
	pio_reset(p);
}

void
pio_reset(pio *p)
{
	for (int i = 0; i < 2; i++) {
		pio_port *pp = &p->p[i];
		pp->mode = 1;		/* input after reset */
		pp->out = 0;
		pp->dir = 0xff;
		pp->icw = 0;		/* interrupts disabled */
		pp->mask = 0xff;
		pp->expect = 0;
		pp->match = 0;
		pp->irq.ip = pp->irq.ius = 0;
	}
}

uint8_t
pio_driven(const pio *p, int port)
{
	const pio_port *pp = &p->p[port];

	switch (pp->mode) {
	case 0: return 0xff;
	case 3: return (uint8_t)~pp->dir;
	default: return 0;
	}
}

uint8_t
pio_pins(const pio *p, int port)
{
	const pio_port *pp = &p->p[port];
	uint8_t drv = pio_driven(p, port);
	return (pp->out & drv) | (pp->pins_in & ~drv);
}

/* bit-control mode interrupt logic */
static void
evaluate(pio_port *pp)
{
	if (pp->mode != 3)
		return;
	uint8_t mon = (uint8_t)~pp->mask & pp->dir;	/* monitored inputs */
	uint8_t lv = pp->pins_in;
	if (!(pp->icw & ICW_HIGH))
		lv = ~lv;				/* active low */
	uint8_t act = lv & mon;
	int m;
	if (!mon)
		m = 0;
	else if (pp->icw & ICW_AND)
		m = act == mon;
	else
		m = act != 0;
	if (m && !pp->match && (pp->icw & ICW_ENABLE))
		pp->irq.ip = 1;
	pp->match = m;
}

void
pio_write_data(pio *p, int port, uint8_t v)
{
	p->p[port].out = v;
}

uint8_t
pio_read_data(pio *p, int port)
{
	pio_port *pp = &p->p[port];

	switch (pp->mode) {
	case 0: return pp->out;
	case 3: return (pp->out & ~pp->dir) | (pp->pins_in & pp->dir);
	default: return pp->pins_in;
	}
}

void
pio_write_ctrl(pio *p, int port, uint8_t v)
{
	pio_port *pp = &p->p[port];

	if (pp->expect == 1) {
		pp->dir = v;
		pp->expect = 0;
		evaluate(pp);
		return;
	}
	if (pp->expect == 2) {
		pp->mask = v;
		pp->expect = 0;
		pp->match = 0;
		evaluate(pp);
		return;
	}
	if (!(v & 1)) {
		pp->vector = v;
		return;
	}
	switch (v & 0x0f) {
	case 0x0f:					/* mode select */
		pp->mode = v >> 6;
		if (pp->mode == 3)
			pp->expect = 1;
		return;
	case 0x07:					/* interrupt control */
		pp->icw = v & 0xf0;
		if (v & 0x10) {
			pp->expect = 2;
			pp->irq.ip = 0;
		} else
			evaluate(pp);
		return;
	case 0x03:					/* interrupt enable only */
		pp->icw = (pp->icw & 0x7f) | (v & 0x80);
		return;
	}
}

void
pio_set_input(pio *p, int port, uint8_t levels)
{
	pio_port *pp = &p->p[port];

	if (pp->pins_in == levels)
		return;
	pp->pins_in = levels;
	evaluate(pp);
}
