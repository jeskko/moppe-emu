#include "sio.h"

#include <string.h>

/* WR1 */
#define WR1_EXTINT   0x01
#define WR1_TXINT    0x02
#define WR1_SAV      0x04	/* status affects vector (channel B WR1) */
#define WR1_RXMODE   0x18
/* WR3 */
#define WR3_RXEN     0x01
/* WR4 */
#define WR4_CLKMODE  0xc0
/* WR5 */
#define WR5_TXEN     0x08

static const char *irqname[2][3] = {
	{ "SIO A RX", "SIO A TX", "SIO A ESC" },
	{ "SIO B RX", "SIO B TX", "SIO B ESC" },
};

static uint8_t
sio_vector(void *ctx, int id)
{
	sio *s = ctx;
	int ch = id >> 2, src = id & 3;
	uint8_t v = s->ch[1].wr[2];

	if (!(s->ch[1].wr[1] & WR1_SAV))
		return v;
	/* status affects vector: V3..V1 */
	static const uint8_t code[2][4] = {
		/* A: RX 110, TX 100, ESC 101, special 111 */
		{ 6, 4, 5, 7 },
		/* B: RX 010, TX 000, ESC 001, special 011 */
		{ 2, 0, 1, 3 },
	};
	return (v & 0xf1) | (code[ch][src] << 1);
}

static void
update_baud(sio *s, int ch)
{
	static const int div[4] = { 1, 16, 32, 64 };
	s->ch[ch].baud = s->clock_hz[ch] / div[s->ch[ch].wr[4] >> 6];
}

void
sio_init(sio *s)
{
	memset(s, 0, sizeof(*s));
	for (int c = 0; c < 2; c++)
		for (int i = 0; i < 3; i++) {
			irqsrc *q = &s->ch[c].irq[i];
			q->name = irqname[c][i];
			q->vector = sio_vector;
			q->ctx = s;
			q->id = (c << 2) | i;
		}
	sio_reset(s);
}

static void
chan_reset(sio *s, int c)
{
	sio_chan *ch = &s->ch[c];
	uint8_t vec = ch->wr[2];

	memset(ch->wr, 0, sizeof(ch->wr));
	if (c == 1)
		ch->wr[2] = vec;	/* vector survives channel reset */
	ch->ptr = 0;
	ch->rxn = 0;
	ch->txbuf_full = 0;
	ch->txshift_busy = 0;
	ch->tx_int_armed = 0;
	ch->latch_valid = 0;
	for (int i = 0; i < 3; i++)
		ch->irq[i].ip = 0;
	update_baud(s, c);
}

void
sio_reset(sio *s)
{
	for (int c = 0; c < 2; c++) {
		chan_reset(s, c);
		for (int i = 0; i < 3; i++)
			s->ch[c].irq[i].ius = 0;
	}
}

void
sio_add_irqs(sio *s, daisy *d)
{
	/* internal priority: A RX, A TX, A ESC, B RX, B TX, B ESC */
	for (int c = 0; c < 2; c++)
		for (int i = 0; i < 3; i++)
			daisy_add(d, &s->ch[c].irq[i]);
}

static uint8_t
rr0(sio *s, int c)
{
	sio_chan *ch = &s->ch[c];
	uint8_t v = ch->latch_valid ? ch->latched : ch->status;

	v &= SIO_RR0_STATUS;
	if (ch->rxn)
		v |= SIO_RR0_RCA;
	if (!ch->txbuf_full)
		v |= SIO_RR0_TBE;
	if (c == 0) {
		for (int cc = 0; cc < 2; cc++)
			for (int i = 0; i < 3; i++)
				if (s->ch[cc].irq[i].ip)
					v |= SIO_RR0_INTP;
	}
	return v;
}

static void
rx_irq_update(sio_chan *ch)
{
	int mode = (ch->wr[1] & WR1_RXMODE) >> 3;

	if (!ch->rxn || mode == 0) {
		ch->irq[SIO_SRC_RX].ip = 0;
		return;
	}
	if (mode == 1) {
		if (ch->first_char) {
			ch->irq[SIO_SRC_RX].ip = 1;
			ch->first_char = 0;
		}
	} else
		ch->irq[SIO_SRC_RX].ip = 1;
}

void
sio_write_ctrl(sio *s, int c, uint8_t v)
{
	sio_chan *ch = &s->ch[c];
	int reg = ch->ptr;

	ch->ptr = 0;
	if (reg != 0) {
		ch->wr[reg] = v;
		switch (reg) {
		case 1:
			rx_irq_update(ch);
			if (!(v & WR1_TXINT))
				ch->irq[SIO_SRC_TX].ip = 0;
			break;
		case 4:
			update_baud(s, c);
			break;
		case 5:
			if (ch->wr5_changed)
				ch->wr5_changed(ch->ctx, c, v);
			break;
		}
		return;
	}
	ch->wr[0] = v;
	ch->ptr = v & 7;
	switch ((v >> 3) & 7) {
	case 1:		/* send abort (SDLC) */
		break;
	case 2:		/* reset ext/status interrupts */
		ch->irq[SIO_SRC_ESC].ip = 0;
		if (ch->latch_valid && ch->latched != ch->status
		    && (ch->wr[1] & WR1_EXTINT)) {
			/* a change happened while latched: report it now */
			ch->latched = ch->status;
			ch->irq[SIO_SRC_ESC].ip = 1;
		} else
			ch->latch_valid = 0;
		break;
	case 3:		/* channel reset */
		chan_reset(s, c);
		if (ch->wr5_changed)
			ch->wr5_changed(ch->ctx, c, 0);
		break;
	case 4:		/* enable int on next rx character */
		ch->first_char = 1;
		break;
	case 5:		/* reset TX int pending */
		ch->irq[SIO_SRC_TX].ip = 0;
		ch->tx_int_armed = 0;
		break;
	case 6:		/* error reset */
		break;
	case 7:		/* return from int (channel A only) */
		break;
	}
}

uint8_t
sio_read_ctrl(sio *s, int c)
{
	sio_chan *ch = &s->ch[c];
	int reg = ch->ptr;

	ch->ptr = 0;
	switch (reg) {
	case 0: return rr0(s, c);
	case 1: return ch->txshift_busy || ch->txbuf_full ? 0x06 : 0x07;	/* all sent */
	case 2: return c == 1 ? sio_vector(s, 0) : 0;	/* approx: base vector */
	default: return 0;
	}
}

void
sio_write_data(sio *s, int c, uint8_t v)
{
	sio_chan *ch = &s->ch[c];

	ch->txbuf = v;
	ch->txbuf_full = 1;
	ch->tx_int_armed = 1;
	ch->irq[SIO_SRC_TX].ip = 0;
}

uint8_t
sio_read_data(sio *s, int c)
{
	sio_chan *ch = &s->ch[c];
	uint8_t v = 0xff;

	if (ch->rxn) {
		v = ch->rxfifo[0];
		memmove(ch->rxfifo, ch->rxfifo + 1, --ch->rxn);
	}
	if (!ch->rxn)
		ch->irq[SIO_SRC_RX].ip = 0;
	else
		rx_irq_update(ch);
	return v;
}

int
sio_rx_ready(const sio *s, int c)
{
	return (s->ch[c].wr[3] & WR3_RXEN) && s->ch[c].rxn < 3;
}

int
sio_rx_byte(sio *s, int c, uint8_t v)
{
	sio_chan *ch = &s->ch[c];

	if (!(ch->wr[3] & WR3_RXEN))
		return 1;		/* receiver off: byte lost silently */
	if (ch->rxn == 3)
		return 0;		/* overrun */
	ch->rxfifo[ch->rxn++] = v;
	rx_irq_update(ch);
	return 1;
}

void
sio_set_status(sio *s, int c, uint8_t mask, uint8_t levels)
{
	sio_chan *ch = &s->ch[c];
	uint8_t nv = (ch->status & ~mask) | (levels & mask);
	uint8_t changed = nv ^ ch->status;

	if (!changed)
		return;
	if (!ch->latch_valid) {
		/* the first change after a reset freezes RR0 status bits */
		ch->latched = nv;
		ch->latch_valid = 1;
		if (ch->wr[1] & WR1_EXTINT)
			ch->irq[SIO_SRC_ESC].ip = 1;
	}
	ch->status = nv;
}

void
sio_tick(sio *s, double now)
{
	for (int c = 0; c < 2; c++) {
		sio_chan *ch = &s->ch[c];

		if (ch->txshift_busy && now >= ch->txdone_at)
			ch->txshift_busy = 0;
		if (ch->txbuf_full && !ch->txshift_busy && (ch->wr[5] & WR5_TXEN)) {
			/* move buffer to shifter */
			ch->txbuf_full = 0;
			ch->txshift_busy = 1;
			double bits = 10;	/* start + 8 data + stop */
			ch->txdone_at = now + (ch->baud > 0 ? bits / ch->baud : 0);
			if (ch->tx_byte)
				ch->tx_byte(ch->ctx, c, ch->txbuf);
			if (ch->tx_int_armed && (ch->wr[1] & WR1_TXINT))
				ch->irq[SIO_SRC_TX].ip = 1;
		}
	}
}
