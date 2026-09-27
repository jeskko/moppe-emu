/*
 * Z80-family interrupt daisy chain (IM2).
 *
 * Every interrupt source (a PIO port, one of the SIO's six sources, ...)
 * is an irqsrc registered in priority order, highest first.  A source may
 * request when nothing of higher priority is under service (IUS).  On
 * acknowledge the requesting source moves from IP to IUS and supplies the
 * vector; RETI clears the highest-priority IUS.
 */
#ifndef DAISY_H
#define DAISY_H

#include <stdint.h>

typedef struct irqsrc irqsrc;

struct irqsrc {
	const char *name;
	uint8_t ip;		/* interrupt pending */
	uint8_t ius;		/* interrupt under service */
	/* vector supplier, called at acknowledge */
	uint8_t (*vector)(void *ctx, int id);
	void   *ctx;
	int     id;
	/* optional: called at acknowledge (e.g. SIO clears state) */
	void  (*acked)(void *ctx, int id);
};

#define DAISY_MAX 16

typedef struct daisy {
	irqsrc *src[DAISY_MAX];
	int     n;
} daisy;

void    daisy_add(daisy *d, irqsrc *s);
int     daisy_int_line(const daisy *d);
uint8_t daisy_ack(daisy *d);
void    daisy_reti(daisy *d);
void    daisy_reset(daisy *d);

#endif
