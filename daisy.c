#include "daisy.h"

void
daisy_add(daisy *d, irqsrc *s)
{
	if (d->n < DAISY_MAX)
		d->src[d->n++] = s;
}

static irqsrc *
requester(const daisy *d)
{
	for (int i = 0; i < d->n; i++) {
		irqsrc *s = d->src[i];
		if (s->ius)
			return 0;	/* blocks itself and everything below */
		if (s->ip)
			return s;
	}
	return 0;
}

int
daisy_int_line(const daisy *d)
{
	return requester(d) != 0;
}

uint8_t
daisy_ack(daisy *d)
{
	irqsrc *s = requester(d);

	if (!s)
		return 0xff;	/* spurious: bus floats high */
	s->ip = 0;
	s->ius = 1;
	uint8_t v = s->vector(s->ctx, s->id);
	if (s->acked)
		s->acked(s->ctx, s->id);
	return v;
}

void
daisy_reti(daisy *d)
{
	for (int i = 0; i < d->n; i++)
		if (d->src[i]->ius) {
			d->src[i]->ius = 0;
			return;
		}
}

void
daisy_reset(daisy *d)
{
	for (int i = 0; i < d->n; i++)
		d->src[i]->ip = d->src[i]->ius = 0;
}
