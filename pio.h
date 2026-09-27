/*
 * Z80 PIO.  Ports A and B; mode 3 (bit control) is what the R58 firmware
 * uses, modes 0/1 are modelled without handshake timing.
 *
 * The board sets input pin levels with pio_set_input(); for bit-control
 * mode the chip evaluates the interrupt logic function on every change.
 */
#ifndef PIO_H
#define PIO_H

#include <stdint.h>
#include "daisy.h"

typedef struct pio_port {
	uint8_t mode;		/* 0 out, 1 in, 2 bidir, 3 bit control */
	uint8_t out;		/* output register */
	uint8_t dir;		/* mode 3: 1 = input */
	uint8_t pins_in;	/* levels driven onto the pins from outside */
	uint8_t vector;
	uint8_t icw;		/* interrupt control word bits 7..4 */
	uint8_t mask;		/* 1 = not monitored */
	uint8_t expect;		/* 0 normal, 1 dir follows, 2 mask follows */
	uint8_t match;		/* last value of the logic function */
	irqsrc  irq;
} pio_port;

typedef struct pio {
	pio_port p[2];
} pio;

void    pio_init(pio *p);
void    pio_reset(pio *p);
/* port: 0 = A, 1 = B */
void    pio_write_data(pio *p, int port, uint8_t v);
uint8_t pio_read_data(pio *p, int port);
void    pio_write_ctrl(pio *p, int port, uint8_t v);
void    pio_set_input(pio *p, int port, uint8_t levels);
/* what the pins look like from outside: outputs driven, inputs = pins_in */
uint8_t pio_pins(const pio *p, int port);
/* output-enable mask (1 = pin driven by the PIO) */
uint8_t pio_driven(const pio *p, int port);

#endif
