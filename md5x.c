/*
 * Talkman MD50 / MD59 / ME59 board.  See md5x.h and notes/md5x.md.
 */
#include "md5x.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* output latch bits, as latch*4 + bit */
#define O_SE     (0 * 4 + 0)	/* MD5x SE, ME59 TSE (offset PLL) */
#define O_PHI    (1 * 4 + 2)
#define O_CS     2		/* latch 2 bits 1..3 = CS1..CS3 */
#define O_CS3    (2 * 4 + 3)	/* selects the programming module */
#define O_RSE    (3 * 4 + 0)	/* ME59 main PLL strobe */
#define O_HS     4		/* latch 4: bit 1 DP, bit 2 CLK */
#define O_WDR    (5 * 4 + 0)
#define O_SWE    (6 * 4 + 0)
#define O_PSC    (6 * 4 + 3)
#define O_EDI    (6 * 4 + 2)	/* MD59/ME59 EEPROM DI */
#define O_EECS   (4 * 4 + 0)	/* MD59/ME59 EEPROM CS (MD50: OFF1) */
#define O_XM     (7 * 4 + 0)

#define LBIT(m, b) (((m)->latch[(b) >> 2] >> ((b) & 3)) & 1)

/* modem */
#define ST_RFLAG 0x01
#define ST_HM    0x02
#define ST_TFLAG 0x04
#define ST_TXE   0x08
#define ST_CFLAG 0x20
#define ST_GPIN1 0x40
#define ST_GPIN2 0x80
#define CT_RINTE 0x01
#define CT_TINTE 0x02
#define CT_CINTE 0x04
#define CT_SHM   0x10
#define CT_CHM   0x20
#define CT_CCF   0x40
#define CT_CRTF  0x80

#define MODEM_HZ 3686400.0	/* modem crystal: 100 Hz timer, 19.2 kHz */

/* 4021 inputs after DA, in shift order (P7..P1, then serial in), with
 * the pin level that means "active".  From the firmware's reads
 * (mx5x.asm input_register); MD50 matches the PE2A/AP1 text. */
typedef struct { int8_t sig; uint8_t active; } pin_t;

static const pin_t pins[3][8] = {
	[MD5X_MD50] = {
		{ MD5X_IN_POWEROFF, 1 }, { MD5X_IN_TOFF, 1 }, { MD5X_IN_OFFHOOK, 1 },
		{ MD5X_IN_LOWBATT, 1 }, { MD5X_IN_PTT, 0 } /* PC */, { MD5X_IN_LOCAL, 0 },
		{ MD5X_IN_PORTABLE, 1 }, { MD5X_IN_COLDSTART, 1 },
	},
	[MD5X_MD59] = {
		{ MD5X_IN_POWEROFF, 1 }, { MD5X_IN_PTT, 1 } /* AC */, { MD5X_IN_OFFHOOK, 1 },
		{ MD5X_IN_LOWBATT, 1 }, { MD5X_IN_IGN, 0 }, { MD5X_IN_LOCAL, 1 },
		{ MD5X_IN_PORTABLE, 0 }, { MD5X_IN_COLDSTART, 1 },
	},
	[MD5X_ME59] = {
		{ MD5X_IN_POWEROFF, 1 }, { MD5X_IN_TOFF, 1 }, { MD5X_IN_OFFHOOK, 1 },
		{ MD5X_IN_PTT, 0 }, { MD5X_IN_PORTABLE, 0 }, { MD5X_IN_AC, 1 },
		{ MD5X_IN_IGN, 0 }, { MD5X_IN_LOCAL, 1 },
	},
};

static void
event(md5x *m, int type, int arg)
{
	unsigned i = (m->ev_head + m->ev_n) % MD5X_EVQ;
	if (m->ev_n == MD5X_EVQ) {
		m->ev_head = (m->ev_head + 1) % MD5X_EVQ;
		m->ev_n--;
	}
	m->ev[i].at = m->clk;
	m->ev[i].type = type;
	m->ev[i].arg = arg;
	m->ev_n++;
}

int
md5x_event_pop(md5x *m, md5x_event *e)
{
	if (!m->ev_n)
		return 0;
	*e = m->ev[m->ev_head];
	m->ev_head = (m->ev_head + 1) % MD5X_EVQ;
	m->ev_n--;
	return 1;
}

static void
iolog(md5x *m, int kind, int n, uint16_t a, uint8_t v)
{
	if (!m->iolog)
		return;
	md5x_io *e = &m->iolog[m->io_n++ % MD5X_IOLOG];
	e->at = m->clk;
	e->pc = m->pc;
	e->addr = a;
	e->kind = (uint8_t)kind;
	e->n = (uint8_t)n;
	e->val = v;
}

void
md5x_iolog(md5x *m, int on, int memw)
{
	if (on && !m->iolog)
		m->iolog = calloc(MD5X_IOLOG, sizeof(*m->iolog));
	else if (!on) {
		free(m->iolog);
		m->iolog = NULL;
	}
	m->io_n = 0;
	m->io_memw = (uint8_t)memw;
}

/* ---------------------------------------------------------------- inputs */

/* TOFF ("no RF output") follows the transmitter: active while XM is
 * off, or when the host forces a TX fault with MD5X_IN_TOFF.  OH1E's
 * txoff waits for it after dropping XM. */
static int
toff(const md5x *m)
{
	return m->in[MD5X_IN_TOFF] || !LBIT(m, O_XM);
}

static int
pin_level(const md5x *m, int k)
{
	const pin_t *p = &pins[m->model][k];
	int on = p->sig == MD5X_IN_TOFF ? toff(m) : m->in[p->sig];
	return on ? p->active : !p->active;
}

static uint8_t
parallel_inputs(const md5x *m)
{
	uint8_t v = (uint8_t)(cu53an_da(&m->hs) << 7);	/* P8 = DA */
	for (int k = 0; k < 7; k++)
		v |= (uint8_t)(pin_level(m, k) << (6 - k));
	return v;
}

/* programming module: after CLK edge c (1-based), group (c-1)/5,
 * position (c-1)%5; position 0 loads the nibble, 1..4 show bits 3..0 */
static int
progmodule_dcu(const md5x *m)
{
	if (!m->pm_n || !m->pm_clk)
		return 1;
	unsigned g = (m->pm_clk - 1) / 5, p = (m->pm_clk - 1) % 5;
	if (!p || g >= m->pm_n)
		return 1;
	return (m->pm_nib[g] >> (4 - p)) & 1;
}

void
md5x_prom(md5x *m, const uint8_t *nibbles, int n)
{
	memset(m->prom, 0, sizeof(m->prom));	/* blank: all bits 0 */
	for (int i = 0; i < n && i < 256; i++)
		m->prom[i] = nibbles[i] & 15;
	m->prom_loaded = 1;
}

void
md5x_progmodule(md5x *m, const uint8_t *nibbles, int n)
{
	if (n > (int)sizeof(m->pm_nib))
		n = (int)sizeof(m->pm_nib);
	m->pm_n = (uint8_t)(n > 0 ? n : 0);
	for (int i = 0; i < m->pm_n; i++)
		m->pm_nib[i] = nibbles[i] & 15;
	m->pm_clk = 0;
}

static uint8_t
ef(void *ctx)
{
	md5x *m = ctx;
	uint8_t f = 0;
	/* /EF1 = DCU (handset serial data, or the programming module
	 * while CS3 is set; nothing there pulls it high) */
	if (LBIT(m, O_CS3) ? !progmodule_dcu(m) : !cu53an_dcu(&m->hs))
		f |= 1;
	/* /EF2: MD59 RF_OFF (TOFF, low = output present, per OH1E's
	 * input_register); MD50 high (production-test link open); ME59
	 * EEPROM data, idle high */
	if (m->model == MD5X_MD59 && !toff(m))
		f |= 2;
	/* 93C06 DO: MD59 on /EF4 with /PTT (OH3NWQ; the original MD59
	 * firmware reads it there), ME59 on /EF2 (manual); high-Z with a
	 * pull-up while CS is low */
	if (m->model != MD5X_MD50 && m->ee.cs && !m->ee.dout)
		f |= m->model == MD5X_MD59 ? 8 : 2;
	/* /EF3 = 4021 Q8; PL is asynchronous, so Q8 = DA while PSC = 1 */
	uint8_t q8 = LBIT(m, O_PSC) ? (uint8_t)cu53an_da(&m->hs) : m->sr >> 7;
	if (!q8)
		f |= 4;
	/* /EF4 = /PTT */
	if (m->in[MD5X_IN_PTT])
		f |= 8;
	return f;
}

/* ---------------------------------------------------------------- 93C06 */

enum { EE_IDLE, EE_CMD, EE_READ, EE_WDATA, EE_WRAL, EE_DONE };

static void
ee_write(md5x *m, int a, uint16_t v)
{
	m->ee.word[a & 15] = v;
	event(m, MD5X_EV_EEPROM, a & 15);
}

/* SK rising edge (Q) while CS is high.  Instruction: start bit 1, two
 * opcode bits, six address bits (A5, A4 ignored), as on the 93C46. */
static void
ee_clock(md5x *m)
{
	md5x_eeprom *e = &m->ee;
	int di = LBIT(m, O_EDI);
	switch (e->state) {
	case EE_IDLE:
		if (di) {
			e->state = EE_CMD;
			e->n = 0;
			e->sr = 0;
		}
		break;
	case EE_CMD:
		e->sr = (uint16_t)((e->sr << 1) | di);
		if (++e->n < 8)
			break;
		e->addr = e->sr & 15;
		e->n = 0;
		switch ((e->sr >> 6) & 3) {
		case 2:					/* READ */
			e->state = EE_READ;
			e->sr = e->word[e->addr];
			e->dout = 0;			/* dummy 0 */
			break;
		case 1:					/* WRITE */
			e->state = EE_WDATA;
			e->sr = 0;
			break;
		case 3:					/* ERASE */
			if (e->ewen)
				ee_write(m, e->addr, 0xffff);
			e->state = EE_DONE;
			break;
		default:
			switch ((e->sr >> 4) & 3) {
			case 3: e->ewen = 1; e->state = EE_DONE; break;
			case 0: e->ewen = 0; e->state = EE_DONE; break;
			case 2:				/* ERAL */
				if (e->ewen)
					for (int a = 0; a < 16; a++)
						ee_write(m, a, 0xffff);
				e->state = EE_DONE;
				break;
			case 1:				/* WRAL */
				e->state = EE_WRAL;
				e->sr = 0;
				break;
			}
		}
		break;
	case EE_READ:
		e->dout = (e->sr >> 15) & 1;
		e->sr = (uint16_t)(e->sr << 1);
		if (++e->n == 16) {			/* sequential read */
			e->n = 0;
			e->addr = (e->addr + 1) & 15;
			e->sr = e->word[e->addr];
		}
		break;
	case EE_WDATA:
	case EE_WRAL:
		e->sr = (uint16_t)((e->sr << 1) | di);
		if (++e->n < 16)
			break;
		if (e->ewen) {
			if (e->state == EE_WRAL)
				for (int a = 0; a < 16; a++)
					ee_write(m, a, e->sr);
			else
				ee_write(m, e->addr, e->sr);
		}
		e->state = EE_DONE;
		e->dout = 1;				/* ready (writes are instant) */
		break;
	}
}

static void
ee_cs(md5x *m, int cs)
{
	md5x_eeprom *e = &m->ee;
	if (cs == e->cs)
		return;
	e->cs = (uint8_t)cs;
	e->state = EE_IDLE;
	e->dout = 1;
}

static void
qout(void *ctx, int level)
{
	md5x *m = ctx;
	if (level && m->model != MD5X_MD50 && m->ee.cs)
		ee_clock(m);
	/* Q clocks the 4021: shift on the rising edge while PSC = 0 */
	if (level && !LBIT(m, O_PSC))
		m->sr = (uint8_t)((m->sr << 1) | pin_level(m, 7));
}

/* ---------------------------------------------------------------- modem */

static void
modem_reset(md5x *m)
{
	m->modem.ctrl = 0;
	m->modem.stat = ST_TFLAG;
	m->modem.hunt = 1;		/* no bytes before a frame sync */
	m->modem.tx_hold_full = 0;
	m->modem.tx_n = 0;
}

static int air_heard(const md5x *m);

static uint8_t
modem_status(md5x *m)
{
	uint8_t s = m->modem.stat & ~(ST_GPIN1 | ST_GPIN2 | ST_HM);
	if (m->modem.hunt)
		s |= ST_HM;
	int carrier = air_heard(m);
	if (m->rx_tone_hz > 0) {
		double t = (double)m->clk / m->cpu_hz;
		if ((uint64_t)(t * 2.0 * m->rx_tone_hz) & 1)
			s |= ST_GPIN1;
	} else if (carrier) {
		/* clipped FFSK: 1 = one cycle of 1200 Hz, 0 = 1.5 of 1800 Hz */
		double dt = (double)(m->clk - m->air.bit_start) / m->cpu_hz;
		double ph = m->air.phase + dt * (m->air.bit ? 1200.0 : 1800.0);
		if (ph - (double)(uint64_t)ph < 0.5)
			s |= ST_GPIN1;
	}
	if (m->model == MD5X_ME59) {
		if (m->pit.c[2].out)
			s |= ST_GPIN2;
	} else if (m->squelch_open || carrier) {
		s |= ST_GPIN2;
	}
	return s;
}

/* the phone is tuned to the base station and it is transmitting */
static int
air_heard(const md5x *m)
{
	return m->air.div && m->air.bit >= 0 && m->synth.loads &&
	       m->synth.div == m->air.div;
}

static void update_int(md5x *m);

/* one bit out of the ideal demodulator */
static void
modem_rx_bit(md5x *m, int b)
{
	md5x_modem *d = &m->modem;
	d->sync_sr = (uint16_t)(((d->sync_sr << 1) | b) & 0x7ff);
	if (d->hunt) {
		if (d->sync_sr == 0x712) {		/* 11100010010 */
			d->hunt = 0;
			d->rx_n = 0;
			d->rx_bytes = 0;
		}
		return;
	}
	d->rx_sr = (uint8_t)((d->rx_sr >> 1) | (b << 7));	/* first bit in bit 0 */
	if (++d->rx_n == 8) {
		d->rx_n = 0;
		d->rx = d->rx_sr;
		d->stat |= ST_RFLAG;
		/* a frame is 140 bits; the original MD59 firmware takes 19
		 * bytes per frame (18 stored, one dropped) without setting
		 * hunt mode again, so the chip goes back to hunting itself */
		if (++d->rx_bytes == 19)
			d->hunt = 1;
		update_int(m);
	}
}

/* record one bit period of the phone's transmitter for the host */
static void
air_tx_slot(md5x *m, uint8_t slot)
{
	md5x_air *a = &m->air;
	if (a->tx_n == MD5X_AIR) {
		a->tx_head = (a->tx_head + 1) % MD5X_AIR;
		a->tx_n--;
		a->tx_lost++;
	}
	a->tx[(a->tx_head + a->tx_n++) % MD5X_AIR] = slot;
}

/* one bit period of the transmitter: load the holding register when the
 * shift register is empty (TFLAG), send LSB first, record the slot */
static void
modem_tx_bit(md5x *m)
{
	md5x_modem *d = &m->modem;
	md5x_air *a = &m->air;
	if (!m->powered) {
		air_tx_slot(m, MD5X_TX_OFF);
		return;
	}
	if (!d->tx_n && d->tx_hold_full) {
		d->tx_sr = d->tx;
		d->tx_n = 8;
		d->tx_hold_full = 0;
		d->stat |= ST_TFLAG;
		update_int(m);
	}
	uint8_t slot = LBIT(m, O_XM) ? MD5X_TX_IDLE : MD5X_TX_OFF;
	if (d->tx_n) {
		if (slot == MD5X_TX_IDLE)
			slot = d->tx_sr & 1;
		d->tx_sr >>= 1;
		if (!--d->tx_n && !d->tx_hold_full)
			d->stat &= ~ST_TXE;
	}
	(void)a;
	air_tx_slot(m, slot);
}

int
md5x_tx_take(md5x *m, uint8_t *out, int max)
{
	md5x_air *a = &m->air;
	int k = 0;
	for (; k < max && a->tx_n; k++, a->tx_n--) {
		out[k] = a->tx[a->tx_head];
		a->tx_head = (a->tx_head + 1) % MD5X_AIR;
	}
	return k;
}

int
md5x_air_push(md5x *m, const uint8_t *bits, int n)
{
	int k = 0;
	for (; k < n && m->air.n < MD5X_AIR; k++, m->air.n++)
		m->air.bits[(m->air.head + m->air.n) % MD5X_AIR] = bits[k] > 1 ? 2 : bits[k];
	return k;
}

void
md5x_air_channel(md5x *m, uint32_t div)
{
	m->air.div = div;
}

/* bit boundaries up to now: next bit from the ring (carrier off when it
 * is empty), into the demodulator while tuned */
static void
air_advance(md5x *m)
{
	md5x_air *a = &m->air;
	while (m->clk >= a->next) {
		if (a->bit >= 0)
			a->phase += a->bit ? 1.0 : 1.5;
		a->phase -= (double)(uint64_t)a->phase;
		a->bit_start = a->next;
		a->nbits++;
		a->next = (uint64_t)((double)(a->nbits + 1) * m->cpu_hz / 1200.0);
		if (a->n) {
			a->bit = a->bits[a->head] > 1 ? -1 : a->bits[a->head];
			a->head = (a->head + 1) % MD5X_AIR;
			a->n--;
		} else {
			a->bit = -1;
		}
		if (m->powered && air_heard(m))
			modem_rx_bit(m, a->bit);
		modem_tx_bit(m);
	}
}

static void
update_int(md5x *m)
{
	uint8_t s = m->modem.stat, c = m->modem.ctrl;
	m->cpu.int_line = ((s & ST_CFLAG) && (c & CT_CINTE)) ||
	                  ((s & ST_RFLAG) && (c & CT_RINTE)) ||
	                  ((s & ST_TFLAG) && (c & CT_TINTE));
}

void
md5x_modem_rx(md5x *m, uint8_t byte)
{
	m->modem.rx = byte;
	m->modem.stat |= ST_RFLAG;
	update_int(m);
}

/* ---------------------------------------------------------------- synth */

static void
synth_strobe(md5x *m, int which)
{
	md5x_synth *s = &m->synth;
	uint32_t b = s->bits;
	s->last_nbits = s->nbits;
	if (which == 0 && m->model == MD5X_ME59) {
		/* SW1 SW2 b16..b0 */
		s->div = b & 0x1ffff;
		s->sw = (b >> 17) & 3;
		s->loads++;
	} else if (which == 0) {
		/* SW1 SW2 N9..N0 A6..A0 */
		s->a = b & 0x7f;
		s->n = (b >> 7) & 0x3ff;
		s->sw = (b >> 17) & 3;
		s->div = s->n * 80 + s->a;
		s->loads++;
	} else {
		s->off_a = b & 0x7f;
		s->off_n = (b >> 7) & 0x3ff;
		s->off_sw = (b >> 17) & 3;
		s->off_loads++;
	}
	event(m, MD5X_EV_SYNTH, which);
}

/* ---------------------------------------------------------------- latches */

static void
handset_bus(md5x *m)
{
	uint8_t sel = (m->latch[O_CS] >> 1) & 3;
	uint8_t dp = (m->latch[O_HS] >> 1) & 1, clk = (m->latch[O_HS] >> 2) & 1;
	unsigned frames = m->hs.frames;
	cu53an_out2(&m->hs, (uint8_t)((sel << 4) | (clk << 6) | (dp << 7)));
	if (m->hs.frames != frames)
		event(m, MD5X_EV_LCD, 0);
}

static void
power_off(md5x *m)
{
	if (!m->powered)
		return;
	m->powered = 0;
	event(m, MD5X_EV_POWEROFF, 0);
}

static void
latch_write(md5x *m, int n, uint8_t v)
{
	uint8_t old = m->latch[n];
	v &= 15;
	m->latch[n] = v;
	uint8_t rose = v & ~old, chg = v ^ old;
	if (!chg)
		return;

	if (m->aud_on && (m->aud_bit >> 2) == n && (chg >> (m->aud_bit & 3)) & 1) {
		if (m->aud_n == m->aud_cap) {
			m->aud_cap = m->aud_cap ? m->aud_cap * 2 : 4096;
			m->aud_t = realloc(m->aud_t, m->aud_cap * sizeof(*m->aud_t));
			m->aud_v = realloc(m->aud_v, m->aud_cap);
		}
		m->aud_t[m->aud_n] = m->clk;
		m->aud_v[m->aud_n++] = (v >> (m->aud_bit & 3)) & 1;
	}

	switch (n) {
	case 0:
		if (rose & 1)
			synth_strobe(m, m->model == MD5X_ME59 ? 1 : 0);
		break;
	case 2:
	case 4:
		if (n == 2 && (rose & 8))
			m->pm_clk = 0;			/* module selected */
		if (LBIT(m, O_CS3)) {
			if (n == 4 && (rose & 4))
				m->pm_clk++;
		} else {
			handset_bus(m);
		}
		if (n == 4 && m->model == MD5X_MD50 && (rose & 1))
			power_off(m);			/* MD50 OFF1 */
		if (n == 4 && m->model != MD5X_MD50)
			ee_cs(m, v & 1);
		break;
	case 3:
		if (m->model == MD5X_ME59 && (rose & 1))
			synth_strobe(m, 0);		/* RSE */
		if (m->model == MD5X_MD59 && (rose & 2))
			power_off(m);			/* MD59 OFF1 */
		break;
	case 5:
		if (v & 1)
			m->wd_since = m->clk;
		break;
	case 6:
		if (rose & 1) {				/* SWE: new frame */
			m->synth.bits = 0;
			m->synth.nbits = 0;
		}
		if (rose & 8)				/* PSC: parallel load */
			m->sr = parallel_inputs(m);
		if ((v & 8) == 0 && (old & 8))
			m->sr = parallel_inputs(m);	/* latched as PL drops */
		break;
	case 7:
		if (chg & 1)
			event(m, (v & 1) ? MD5X_EV_TX_ON : MD5X_EV_TX_OFF, 0);
		if (m->model == MD5X_ME59 && (rose & 2))
			power_off(m);			/* ME59 OFF1 */
		break;
	}
}

/* ---------------------------------------------------------------- bus */

static uint8_t *
ram_at(md5x *m, uint16_t a)
{
	if (m->model == MD5X_MD50) {
		if (a >= 0x8000 && a < 0xc000)
			return &m->ram[a & (m->ram_size - 1)];
	} else if (a >= 0x8000 && a < 0xa000) {
		return &m->ram[a & (m->ram_size - 1)];
	}
	return NULL;
}

static uint8_t
mem_read(void *ctx, uint16_t a)
{
	md5x *m = ctx;
	if (a < 0x8000)
		return m->rom[a];
	uint8_t *r = ram_at(m, a);
	if (r)
		return *r;
	if (m->model == MD5X_MD50 && a >= 0xc000) {
		/* 74S287: A0-A3 from the bus, A4-A6 from latch 7 bits 1-3,
		 * A7 from latch 6 bit 1 (as the original firmware drives
		 * them, 0x4CB3); four data bits */
		if (!m->prom_loaded)
			return 0xff;
		int hi = (m->latch[7] >> 1 & 7) | (m->latch[6] >> 1 & 1) << 3;
		return (uint8_t)(0xf0 | (m->prom[hi << 4 | (a & 15)] & 15));
	}
	if (m->model == MD5X_ME59 && a >= 0xe000)
		return m->adc_result;
	return 0xff;
}

static void
mem_write(void *ctx, uint16_t a, uint8_t v)
{
	md5x *m = ctx;

	/* SWE gates MWR onto SCLK and D7 (MD5x) / D0 (ME59) onto SD; the
	 * firmware writes the bits inverted */
	if (LBIT(m, O_SWE)) {
		int bit = m->model == MD5X_ME59 ? (v & 1) : (v >> 7);
		m->synth.bits = (m->synth.bits << 1) | (uint32_t)!bit;
		m->synth.nbits++;
	}
	uint8_t *r = ram_at(m, a);
	if (r) {
		*r = v;
		if (a >= m->watch_lo && a <= m->watch_hi) {
			m->watch_hit = 1;
			m->watch_addr = a;
		}
		return;
	}
	if (m->io_memw)
		iolog(m, MD5X_IO_MEMW, 0, a, v);
	if (m->model == MD5X_ME59) {
		if (a >= 0xe000) {
			m->adc_result = m->adc[a & 7];	/* start conversion */
		} else if (a >= 0xa000 && a < 0xc000) {
			m->dac = v;
			event(m, MD5X_EV_DAC, v);
		}
	}
}

static uint8_t io_read(md5x *m, int n, uint16_t a);

static uint8_t
io_in(void *ctx, int n, uint16_t a)
{
	md5x *m = ctx;
	uint8_t v = io_read(m, n, a);
	iolog(m, MD5X_IO_INP, n, a, v);
	return v;
}

static uint8_t
io_read(md5x *m, int n, uint16_t a)
{
	uint8_t v = 0xff;
	if (n & 1) {
		if (m->model == MD5X_ME59)
			v &= pit_read(&m->pit, a & 3);
		else if (m->model == MD5X_MD50)
			m->waiting = 1;
	}
	if (n & 2) {
		if (a & 0x20) {
			v &= modem_status(m);
		} else {
			v &= m->modem.rx;
			m->modem.stat &= ~ST_RFLAG;
			update_int(m);
		}
	}
	return v;
}

static void
io_out(void *ctx, int n, uint16_t a, uint8_t v)
{
	md5x *m = ctx;
	iolog(m, MD5X_IO_OUT, n, a, v);
	if (n & 1) {
		if (m->model == MD5X_ME59)
			pit_write(&m->pit, a & 3, v);
		else if (m->model == MD5X_MD50)
			m->waiting = 1;
	}
	if (n & 2) {
		if (a & 0x20) {
			m->modem.ctrl = v;
			if (v & CT_CCF)
				m->modem.stat &= ~ST_CFLAG;
			if (v & CT_CRTF)
				m->modem.stat &= ~ST_RFLAG;
			if (v & CT_SHM) {		/* new hunt: drop a pending byte */
				m->modem.hunt = 1;
				m->modem.stat &= ~ST_RFLAG;
			}
			if (v & CT_CHM)
				m->modem.hunt = 0;
		} else {
			m->modem.tx = v;
			m->modem.tx_hold_full = 1;
			m->modem.stat &= ~ST_TFLAG;
			m->modem.stat |= ST_TXE;
			event(m, MD5X_EV_MODEM_TX, v);
		}
		update_int(m);
	}
	if (n & 4)
		latch_write(m, (a >> 1) & 7, v);
}

/* ---------------------------------------------------------------- setup */

static void
board_reset(md5x *m)
{
	/* /CLEAR: CPU, modem, watchdog, output latches */
	cdp1802_reset(&m->cpu, m->model == MD5X_MD50 ? CDP1802 : CDP1806);
	for (int i = 0; i < 8; i++)
		latch_write(m, i, 0);
	modem_reset(m);
	m->waiting = 0;
	m->wd_since = m->clk;
	update_int(m);
}

void
md5x_init(md5x *m, int model, int cu, unsigned ram_size)
{
	memset(m, 0, sizeof(*m));
	m->model = model;
	m->cu = cu;
	m->cpu_hz = model == MD5X_ME59 ? 4800000.0 : 3686400.0;
	if (!ram_size)
		ram_size = model == MD5X_MD50 ? 1024 : 2048;
	m->ram_size = ram_size > sizeof(m->ram) ? sizeof(m->ram) : ram_size;
	memset(m->rom, 0xff, sizeof(m->rom));
	m->cpu.ctx = m;
	m->cpu.read = mem_read;
	m->cpu.write = mem_write;
	m->cpu.in = io_in;
	m->cpu.out = io_out;
	m->cpu.ef = ef;
	m->cpu.qout = qout;
	cu53an_init(&m->hs);
	m->hs.bit1 = cu == MD5X_CU53;
	pit_init(&m->pit);
	m->pit.quiet = 7;
	m->if_hz = 455000.0;
	/* PE2A text: reset 106 ms after the last WDR; ME59 manual: 213 ms */
	m->wd_timeout_s = model == MD5X_ME59 ? 0.213 : 0.106;
	m->aud_bit = O_PHI;
	m->watch_lo = 1;
	m->watch_hi = 0;
	m->modem.next_tick = (uint64_t)(m->cpu_hz / 100);
	m->air.bit = -1;
	m->air.next = (uint64_t)(m->cpu_hz / 1200.0);
	memset(m->ee.word, 0xff, sizeof(m->ee.word));	/* erased */
	m->ee.dout = 1;
	board_reset(m);
}

int
md5x_load_rom(md5x *m, const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	memset(m->rom, 0xff, sizeof(m->rom));
	size_t n = fread(m->rom, 1, sizeof(m->rom), f);
	fclose(f);
	return n ? (int)n : -1;
}

void
md5x_power(md5x *m, int on)
{
	if (on && !m->powered) {
		m->powered = 1;
		board_reset(m);
		event(m, MD5X_EV_POWERON, 0);
	} else if (!on) {
		power_off(m);
	}
}

double
md5x_time(const md5x *m)
{
	return (double)m->clk / m->cpu_hz;
}

void
md5x_set_input(md5x *m, int which, int v)
{
	if (which >= 0 && which < MD5X_IN_N)
		m->in[which] = !!v;
}

void
md5x_key(md5x *m, int code)
{
	if (code < 0) {
		cu53an_set_key(&m->hs, CU53_KEY_NONE);
		m->hs.bit1 = m->cu == MD5X_CU53;
		return;
	}
	cu53an_set_key(&m->hs, code & 31);
	int extra = (code >> 5) & 1;
	m->hs.bit1 = (uint8_t)(m->cu == MD5X_CU53 ? !extra : extra);
}

uint8_t
md5x_peek(md5x *m, uint16_t a)
{
	if (a < 0x8000)
		return m->rom[a];
	uint8_t *r = ram_at(m, a);
	return r ? *r : 0xff;
}

void
md5x_poke(md5x *m, uint16_t a, uint8_t v)
{
	uint8_t *r = ram_at(m, a);
	if (r)
		*r = v;
	else if (a < 0x8000)
		m->rom[a] = v;
}

void
md5x_audio_capture(md5x *m, int on)
{
	m->aud_on = on;
	if (on)
		m->aud_n = 0;
}

void
md5x_breakpoint(md5x *m, uint16_t a, int on)
{
	uint8_t bit = (uint8_t)(1 << (a & 7));
	if (on && !(m->bp[a >> 3] & bit)) {
		m->bp[a >> 3] |= bit;
		m->nbp++;
	} else if (!on && (m->bp[a >> 3] & bit)) {
		m->bp[a >> 3] &= (uint8_t)~bit;
		m->nbp--;
	}
}

/* ---------------------------------------------------------------- run */

/* peripherals for 'cyc' machine cycles that just ran */
static void
advance(md5x *m, unsigned cyc)
{
	m->clk += (uint64_t)cyc * 8;

	if (m->clk >= m->modem.next_tick) {
		m->modem.stat |= ST_CFLAG;
		m->modem.ticks++;
		m->modem.next_tick = (uint64_t)((double)(m->modem.ticks + 1) * m->cpu_hz / 100.0);
		update_int(m);
	}
	if (m->clk >= m->air.next)
		air_advance(m);
	if (m->model == MD5X_ME59) {
		/* CLK0 = TPB, CLK1 = 455 kHz IF (gate not modelled), CLK2 =
		 * 19.2 kHz from the modem */
		uint64_t hz = (uint64_t)m->cpu_hz;
		pit_clock(&m->pit, 0, cyc);
		m->pit_acc1 += (uint64_t)cyc * 8 * (uint64_t)m->if_hz;
		if (m->pit_acc1 >= hz) {
			pit_clock(&m->pit, 1, (unsigned)(m->pit_acc1 / hz));
			m->pit_acc1 %= hz;
		}
		m->pit_acc2 += (uint64_t)cyc * 8 * 19200;
		if (m->pit_acc2 >= hz) {
			pit_clock(&m->pit, 2, (unsigned)(m->pit_acc2 / hz));
			m->pit_acc2 %= hz;
		}
	}
	if (LBIT(m, O_WDR)) {
		m->wd_since = m->clk;
	} else if ((double)(m->clk - m->wd_since) > m->wd_timeout_s * m->cpu_hz) {
		event(m, MD5X_EV_WDRESET, m->cpu.r[m->cpu.p]);
		board_reset(m);
	}
}

int
md5x_step(md5x *m)
{
	if (!m->powered)
		return 0;
	if (m->waiting) {
		/* MD50 WAIT: the CPU pauses until /INT (the modem timer) */
		if (m->cpu.int_line) {
			m->waiting = 0;
		} else {
			uint64_t to = m->modem.next_tick;
			unsigned cyc = to > m->clk ? (unsigned)((to - m->clk + 7) / 8) : 1;
			advance(m, cyc);
			return (int)cyc;
		}
	}
	uint16_t pc = m->cpu.r[m->cpu.p];
	m->trace[m->trace_pos++ % MD5X_TRACE] = pc;
	m->pc = pc;
	if (pc < 0x8000 && !m->cpu.idle) {
		m->cov[pc >> 3] |= (uint8_t)(1 << (pc & 7));
		uint8_t op = m->rom[pc];
		if (m->iolog && (op & 0xf4) == 0x34)	/* B1-4 / BN1-4 */
			iolog(m, MD5X_IO_EF, (op & 3) + 1, 0, ef(m));
	}
	unsigned cyc = cdp1802_step(&m->cpu);
	if (m->cpu.illegal)
		event(m, MD5X_EV_ILLEGAL, pc);
	advance(m, cyc);
	return (int)cyc;
}

int
md5x_run(md5x *m, double seconds)
{
	uint64_t end = m->clk + (uint64_t)(seconds * m->cpu_hz);
	while (m->clk < end) {
		if (!m->powered) {
			/* the air goes on: the base station's bits pass by
			 * unheard, the transmitter's slots say off */
			m->clk = end;
			air_advance(m);
			return MD5X_STOP_OFF;
		}
		if (m->nbp && !m->cpu.idle) {
			uint16_t pc = m->cpu.r[m->cpu.p];
			if ((m->bp[pc >> 3] >> (pc & 7)) & 1) {
				if (!m->skip_bp) {
					m->skip_bp = 1;
					return MD5X_STOP_BREAK;
				}
			}
			m->skip_bp = 0;
		}
		md5x_step(m);
		if (m->watch_hit) {
			m->watch_hit = 0;
			return MD5X_STOP_WATCH;
		}
	}
	return MD5X_STOP_TIME;
}
