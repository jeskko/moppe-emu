/*
 * Comarco MDR150 board; see mdr150.h.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "mdr150.h"

/* port F pins (boot.s) */
#define PF_SR_STB	0x02
#define PF_CLK		0x04
#define PF_DATA		0x08
#define PF_PLL_STB	0x10
#define PF_POWEL	0x20	/* 4094 output enable */
#define PF_POWL		0x40
#define PF_POWM		0x80

/* 4094 outputs, active low supplies (hamdr.c RFCMD_*) */
#define SR_AUDIO	0x02
#define SR_PLL		0x04
#define SR_VCO		0x08
#define SR_IF		0x10
#define SR_TX		0x20
#define SR_MUTE		0x80

/* port GP / QS pins (boot.s) */
#define GP_SSEL		0x01
#define GP_RTSB		0x02	/* in */
#define QS_DTRB		0x10	/* in */
#define QS_DTRA		0x20	/* in */
#define QS_RTSA		0x40	/* in */

/* ADC channels: AI (demodulated audio) on 0, 2, 4, 6 */
#define AN_RSSI		1
#define AN_BUSY		3
#define AN_UNLOCK	5

static void
event(mdr150 *r, int type, int arg)
{
	mdr150_event *e = &r->ev[r->ev_head];
	e->at = mdr150_time(r);
	e->type = type;
	e->arg = arg;
	r->ev_head = (r->ev_head + 1) % 4096;
	if (r->ev_head == r->ev_tail)
		r->ev_tail = (r->ev_tail + 1) % 4096;
}

double
mdr150_time(const mdr150 *r)
{
	/* the system clock is 16.777 MHz after the firmware's SYNCR write;
	 * before it (a few microseconds) the reset 8.4 MHz */
	return (double)r->chip.clk / 16777216.0;
}

/* ---------------------------------------------------------------- flash */

/* Am29F010: commands on A14..A0 = 5555 / 2AAA; 8 sectors of 16 KB */
static void
flash_write(mdr150 *r, uint32_t off, uint8_t v)
{
	uint32_t ca = off & 0x7FFF;
	if (r->flash_mode == 1) {		/* byte program: only clears bits */
		r->flash[off] &= v;
		r->flash_mode = 0;
		r->flash_cycle = 0;
		r->flash_dirty = 1;
		event(r, MDR_EV_FLASH, (int)off);
		return;
	}
	if (v == 0xF0) {			/* reset */
		r->flash_mode = 0;
		r->flash_cycle = 0;
		return;
	}
	switch (r->flash_cycle) {
	case 0: case 3:
		r->flash_cycle = (ca == 0x5555 && v == 0xAA) ? r->flash_cycle + 1 : 0;
		break;
	case 1: case 4:
		r->flash_cycle = (ca == 0x2AAA && v == 0x55) ? r->flash_cycle + 1 : 0;
		break;
	case 2:
		r->flash_cycle = 0;
		if (ca != 0x5555)
			break;
		if (v == 0xA0)
			r->flash_mode = 1;
		else if (v == 0x80)
			r->flash_cycle = 3;
		else if (v == 0x90)
			r->flash_mode = 2;
		break;
	case 5:
		r->flash_cycle = 0;
		if (v == 0x30) {		/* sector erase */
			memset(r->flash + (off & 0x1C000), 0xFF, 0x4000);
			r->flash_dirty = 1;
			event(r, MDR_EV_FLASH, (int)(0x80000000u | (off & 0x1C000)));
		} else if (v == 0x10 && ca == 0x5555) {	/* chip erase */
			memset(r->flash, 0xFF, sizeof(r->flash));
			r->flash_dirty = 1;
			event(r, MDR_EV_FLASH, (int)0x80000000u);
		}
		break;
	}
}

static uint8_t
flash_read(mdr150 *r, uint32_t off)
{
	if (r->flash_mode == 2) {		/* autoselect: AMD, 29F010 */
		switch (off & 3) {
		case 0: return 0x01;
		case 1: return 0x20;
		default: return 0x00;
		}
	}
	return r->flash[off];
}

/* ---------------------------------------------------------------- bus */

static uint16_t
b_ext_read(void *ctx, uint32_t a, int size, unsigned cs)
{
	mdr150 *r = ctx;
	uint16_t v = 0xFFFF;
	int hits = 0;
	if ((cs & CS_BOOT) && !(a & 0x40000)) {	/* flash: OE = CSBOOT, CE = /A18 */
		v = flash_read(r, a & 0x1FFFF);
		hits++;
	}
	if (cs & (1u << 2)) {			/* RAM: OE = CS2, both chips */
		uint32_t w = a & 0xFFFE;
		uint16_t rv = (uint16_t)(r->ram[w] << 8 | r->ram[w + 1]);
		if (size == 1)
			rv = (a & 1) ? r->ram[w + 1] : r->ram[w];
		v = rv;
		hits++;
	}
	if (!hits || hits > 1)
		event(r, MDR_EV_UNMAPPED, (int)a);
	return v;
}

static void
b_ext_write(void *ctx, uint32_t a, int size, uint16_t v, unsigned cs)
{
	mdr150 *r = ctx;
	uint32_t w = a & 0xFFFE;
	int hits = 0;
	if ((cs & (1u << 10)) && !(a & 0x40000)) {	/* flash: WE = CS10 */
		flash_write(r, a & 0x1FFFF, (uint8_t)v);
		hits++;
	}
	/* RAM: WE of U10 (D8..D15, even bytes) = CS1, of U9 (D0..D7) = CS0 */
	if (cs & (1u << 1)) {
		r->ram[w] = (uint8_t)(size == 2 ? v >> 8 : v);
		hits++;
	}
	if (cs & (1u << 0)) {
		r->ram[w + 1] = (uint8_t)v;
		hits++;
	}
	if (!hits)
		event(r, MDR_EV_UNMAPPED, (int)a);
}

/* ---------------------------------------------------------------- radio module */

static void
radio_update(mdr150 *r)
{
	int on = (r->pf & PF_POWEL) && !(r->sr_out & SR_TX);
	if (on != r->tx_keyed) {
		r->tx_keyed = on;
		event(r, on ? MDR_EV_TX_ON : MDR_EV_TX_OFF, 0);
	}
	r->power_level = r->pf >> 6;
}

/* port F: 4094 and MB1504 share CLK and DATA; each clocks DATA in on the
 * rising CLK edge, the 4094 latches on SR_STB, the MB1504 on PLL_STB
 * (the last bit shifted in selects the reference or the N/A latch) */
static void
portf(mdr150 *r, uint8_t v)
{
	uint8_t old = r->pf, rise = (uint8_t)(v & ~old);
	r->pf = v;
	if (rise & PF_CLK) {
		int bit = (v & PF_DATA) != 0;
		r->sr_shift = (uint8_t)(r->sr_shift << 1 | bit);
		r->pll_shift = r->pll_shift << 1 | (uint32_t)bit;
	}
	if (rise & PF_SR_STB) {
		r->sr_out = r->sr_shift;
		event(r, MDR_EV_SR, r->sr_out);
	}
	if (rise & PF_PLL_STB) {
		uint32_t s = r->pll_shift;
		if (s & 1) {			/* reference: SW, R14..R1, C=1 */
			r->pll_r = s >> 1 & 0x3FFF;
			event(r, MDR_EV_PLL, (int)(0x80000000u | r->pll_r));
		} else {			/* N11..N1, A7..A1, C=0 */
			uint32_t n = s >> 8 & 0x7FF, aa = s >> 1 & 0x7F;
			r->pll_na = n * 64 + aa;
			event(r, MDR_EV_PLL, (int)r->pll_na);
		}
	}
	radio_update(r);
}

double
mdr150_vco_hz(const mdr150 *r)
{
	if (!r->pll_r)
		return 0;
	return MDR_PLL_REF / r->pll_r * r->pll_na;
}

static void
b_pins_out(void *ctx, int port, uint8_t v, uint8_t ddr)
{
	mdr150 *r = ctx;
	switch (port) {
	case HC16_PORTF:
		/* inputs float high through the pull-ups */
		portf(r, (uint8_t)((v & ddr) | ~ddr));
		break;
	case HC16_PORTC:
		if (v != r->leds) {
			r->leds = v;
			event(r, MDR_EV_LEDS, v);
		}
		break;
	case HC16_PORTGP:
		r->ssel = (ddr & GP_SSEL) ? (v & GP_SSEL) != 0 : 1;
		break;
	}
}

/* RS-232 receivers invert: an asserted line reads 0 */
static uint8_t
b_pins_in(void *ctx, int port)
{
	mdr150 *r = ctx;
	uint8_t v = 0xFF;
	switch (port) {
	case HC16_PORTGP:
		if (r->port[1].rts)
			v &= (uint8_t)~GP_RTSB;
		break;
	case HC16_PORTQS:
		if (r->port[1].dtr)
			v &= (uint8_t)~QS_DTRB;
		if (r->port[0].dtr)
			v &= (uint8_t)~QS_DTRA;
		if (r->port[0].rts)
			v &= (uint8_t)~QS_RTSA;
		break;
	case HC16_PORTADA:
		/* digital reads of the ADC pins: BUSY on AN3, UNLOCK on AN5 */
		v = 0;
		if (r->busy)
			v |= 1 << AN_BUSY;
		if (!(r->pll_r && r->pll_na) || (r->sr_out & (SR_PLL | SR_VCO)))
			v |= 1 << AN_UNLOCK;
		if (r->rssi > 512)
			v |= 1 << AN_RSSI;
		break;
	case HC16_PORTE:
	case HC16_PORTF:
		break;
	}
	return v;
}

static uint32_t
rnd(mdr150 *r)
{
	r->noise_seed = r->noise_seed * 1103515245u + 12345u;
	return r->noise_seed >> 16;
}

static int
b_analog(void *ctx, int ch)
{
	mdr150 *r = ctx;
	double t, x = 0;
	int rx_on;
	switch (ch) {
	case AN_RSSI:
		return r->rssi;
	case AN_BUSY:
		return r->busy ? 1023 : 0;
	case AN_UNLOCK:
		return (b_pins_in(ctx, HC16_PORTADA) & (1 << AN_UNLOCK)) ? 1023 : 0;
	case 7:
		return 512;
	}
	/* AI: the receiver's audio, when it is on: the channel's signal, or
	 * noise without one */
	rx_on = !(r->sr_out & SR_IF) && (r->pf & PF_POWEL);
	if (!rx_on)
		return 512;
	t = mdr150_time(r);
	if (r->rx_audio && t >= r->rx_start) {
		long i = (long)((t - r->rx_start) * r->rx_rate);
		if (i < r->rx_len)
			x = r->rx_audio[i] * r->audio_level;
		else
			r->rx_audio = NULL;
	}
	if (!r->rx_audio)
		x = ((double)(rnd(r) & 0xFFFF) / 32768.0 - 1.0) * r->noise_level;
	x += 512;
	if (x < 0) x = 0;
	if (x > 1023) x = 1023;
	return (int)x;
}

static void
b_sci_tx(void *ctx, uint16_t frame)
{
	mdr150 *r = ctx;
	mdr150_port *p = &r->port[r->ssel];
	int next = (p->out_head + 1) % (int)sizeof(p->out);
	if (next == p->out_tail)
		p->out_tail = (p->out_tail + 1) % (int)sizeof(p->out);
	p->out[p->out_head] = (uint8_t)frame;
	p->out_head = next;
}

static void
b_pwm(void *ctx, int ch, int duty)
{
	mdr150 *r = ctx;
	if (ch != 0 || !r->tx_keyed)
		return;
	if (r->pwm_n == r->pwm_cap) {
		int cap = r->pwm_cap ? 2 * r->pwm_cap : 65536;
		r->pwm_at = realloc(r->pwm_at, (size_t)cap * sizeof(*r->pwm_at));
		r->pwm_duty = realloc(r->pwm_duty, (size_t)cap * sizeof(*r->pwm_duty));
		r->pwm_cap = cap;
	}
	r->pwm_at[r->pwm_n] = r->chip.clk;
	r->pwm_duty[r->pwm_n] = (int16_t)duty;
	r->pwm_n++;
}

static void
b_reset(void *ctx, int why)
{
	mdr150 *r = ctx;
	event(r, MDR_EV_RESET, why);
}

/* ---------------------------------------------------------------- api */

void
mdr150_init(mdr150 *r)
{
	hc16z1_board b = {
		r, b_ext_read, b_ext_write, b_pins_out, b_pins_in, b_analog,
		b_sci_tx, b_pwm, b_reset,
	};
	memset(r, 0, sizeof(*r));
	memset(r->flash, 0xFF, sizeof(r->flash));
	r->audio_level = 300;
	r->noise_level = 150;
	r->rssi = 100;
	r->noise_seed = 1;
	r->sr_out = 0xFF;
	r->bp = calloc(0x100000 / 8, 1);
	hc16z1_init(&r->chip, &b);
}

void
mdr150_free(mdr150 *r)
{
	free(r->bp);
	free(r->pwm_at);
	free(r->pwm_duty);
	r->bp = NULL;
	r->pwm_at = NULL;
	r->pwm_duty = NULL;
}

int
mdr150_load_flash(mdr150 *r, const uint8_t *img, int n)
{
	if (n < 0 || n > MDR_FLASH_SIZE)
		return -1;
	memset(r->flash, 0xFF, sizeof(r->flash));
	memcpy(r->flash, img, (size_t)n);
	return 0;
}

void
mdr150_power(mdr150 *r, int on)
{
	r->powered = on != 0;
	if (on) {
		/* the SRAM keeps nothing without its supply */
		memset(r->ram, 0, sizeof(r->ram));
		r->sr_out = 0xFF;
		r->pf = 0xFF;
		r->pll_na = r->pll_r = r->pll_shift = 0;
		r->tx_keyed = 0;
		r->flash_cycle = r->flash_mode = 0;
		hc16z1_reset(&r->chip);
	}
}

static int
bp_hit(mdr150 *r, uint32_t a)
{
	return (r->bp[a >> 3] >> (a & 7)) & 1;
}

int
mdr150_step(mdr150 *r)
{
	cpu16 *c = &r->chip.cpu;
	int n, exc = c->last_exc;
	r->trace[r->trace_n++ & 255] = cpu16_pc20(c);
	c->last_exc = -1;
	n = hc16z1_step(&r->chip);
	if (c->illegal)
		event(r, MDR_EV_ILLEGAL, (int)c->op_addr);
	if (r->log_exc && c->last_exc >= 0 && c->last_exc != exc)
		event(r, MDR_EV_EXC, c->last_exc);
	return n;
}

int
mdr150_run(mdr150 *r, double s)
{
	/* rounded up: a request shorter than a clock still runs one step */
	uint64_t end = r->chip.clk + (uint64_t)ceil(s * 16777216.0);
	int first = 1;
	if (!r->powered)
		return 0;
	while (r->chip.clk < end) {
		uint32_t pc = cpu16_pc20(&r->chip.cpu);
		if (!first && bp_hit(r, pc))
			return 1;
		first = 0;
		mdr150_step(r);
		if (r->watch_hit) {
			r->watch_hit = 0;
			return 2;
		}
	}
	return 0;
}

void
mdr150_set_rx_audio(mdr150 *r, const float *x, int n, double rate)
{
	r->rx_audio = x;
	r->rx_len = n;
	r->rx_rate = rate;
	r->rx_start = mdr150_time(r);
}

void
mdr150_serial_in(mdr150 *r, int port, uint8_t b)
{
	if ((port & 1) == r->ssel)
		hc16z1_sci_rx(&r->chip, b);
}
