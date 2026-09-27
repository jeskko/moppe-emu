/*
 * Intel 8254 programmable interval timer.
 *
 * Gates are assumed permanently high (as on the R58 P8x card), so modes 1
 * and 5 never trigger.  Counters advance with pit_clock(); output edges are
 * reported through a callback with the clock index at which they happen,
 * so the board can reconstruct waveforms (tones, PWM audio).
 */
#ifndef PIT_H
#define PIT_H

#include <stdint.h>

typedef struct pit_counter {
	uint16_t ce;		/* counting element */
	uint16_t cr;		/* count register (last written count) */
	uint8_t  mode, rw, bcd;
	uint8_t  out;
	uint8_t  null;		/* no count written since mode set */
	uint8_t  load;		/* CR -> CE on next clock */
	uint8_t  armed;		/* mode 0/4: terminal count not yet reached */
	uint8_t  wlsb;		/* RW=both: LSB written, MSB expected */
	uint8_t  rmsb;		/* RW=both: LSB read, MSB next */
	uint8_t  latched;	/* count latched */
	uint16_t latch;
	uint8_t  rlatched;	/* latched value partially read */
	uint8_t  status_latched;
	uint8_t  status;
	uint32_t sq_left;	/* mode 3: clocks left in current half */
	uint8_t  pending_high;	/* mode 4 strobe ends at next clock */
} pit_counter;

typedef struct pit {
	pit_counter c[3];
	/* output change callback: counter, new level, clock offset within
	 * the pit_clock() call (0 = first clock of the batch) */
	void (*out_changed)(void *ctx, int counter, int level, unsigned offset);
	void  *ctx;
} pit;

void    pit_init(pit *p);
void    pit_write(pit *p, int addr, uint8_t v);	/* addr 0..3 */
uint8_t pit_read(pit *p, int addr);
void    pit_clock(pit *p, int counter, unsigned nclocks);

#endif
