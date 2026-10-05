/*
 * Hitachi H8/532 on-chip modules; see h8532.h.
 */
#include <string.h>

#include "h8532.h"

/* interrupt vector numbers (address / 4) */
enum {
	V_IRQ0 = 32, V_IRQ1 = 33,
	V_FRT = 36,		/* FRTn ICI = 36 + 4n, OCIA +1, OCIB +2, FOVI +3 */
	V_CMIA = 48, V_CMIB = 49, V_OVI = 50,
	V_ERI = 52, V_RXI = 53, V_TXI = 54,
	V_ADI = 56,
};

/* FRT TCR / TCSR bits */
#define F_ICIE  0x80
#define F_OCIEB 0x40
#define F_OCIEA 0x20
#define F_OVIE  0x10
#define F_ICF   0x80
#define F_OCFB  0x40
#define F_OCFA  0x20
#define F_OVF   0x10
#define F_IEDG  0x02
#define F_CCLRA 0x01

/* SCI */
#define S_TIE  0x80
#define S_RIE  0x40
#define S_TE   0x20
#define S_RE   0x10
#define S_TDRE 0x80
#define S_RDRF 0x40
#define S_ORER 0x20
#define S_FER  0x10
#define S_PER  0x08

/* ADCSR */
#define A_ADF  0x80
#define A_ADIE 0x40
#define A_ADST 0x20
#define A_SCAN 0x10
#define A_CKS  0x08

/* WDT TCSR */
#define W_OVF  0x80
#define W_WT   0x40
#define W_TME  0x20

static int
in_iram(const h8532 *m, uint32_t a)
{
	return (m->ramcr & 0x80) && a >= 0xFB80 && a <= 0xFF7F;
}

/* ---------------------------------------------------------------- ports */

static uint8_t
port_level(const h8532 *m, int p)
{
	return (uint8_t)((m->dr[p] & m->ddr[p]) | (m->pin[p] & ~m->ddr[p]));
}

static void
port_update(h8532 *m, int p)
{
	uint8_t v = port_level(m, p);
	if (p == 1 && (m->t_tcsr & 0x0F)) {	/* P17 is TMO */
		v = (uint8_t)((v & 0x7F) | (m->tmo ? 0x80 : 0));
	}
	if (v == m->last_out[p])
		return;
	m->last_out[p] = v;
	if (m->bus.port_out)
		m->bus.port_out(m->bus.ctx, p, v, m->ddr[p]);
}

static void
set_tmo(h8532 *m, int level)
{
	if (m->tmo == level)
		return;
	m->tmo = (uint8_t)level;
	if (m->bus.tmo)
		m->bus.tmo(m->bus.ctx, level);
	port_update(m, 1);
}

/* ------------------------------------------------------------------ FRT */

static void
frt_capture(h8532 *m, int n)
{
	h8532_frt *f = &m->frt[n];
	f->icr = f->frc;
	f->tcsr |= F_ICF;
}

static void
frt_tick(h8532 *m, int n)
{
	h8532_frt *f = &m->frt[n];
	/* a match is flagged as the counter leaves the matching value */
	if (f->frc == f->ocra) {
		f->tcsr |= F_OCFA;
		if (f->tcsr & F_CCLRA) {
			f->frc = 0;
			return;
		}
	}
	if (f->frc == f->ocrb)
		f->tcsr |= F_OCFB;
	if (++f->frc == 0)
		f->tcsr |= F_OVF;
}

static void
frt_run(h8532 *m, int n, int states)
{
	static const int div[3] = { 4, 8, 32 };
	h8532_frt *f = &m->frt[n];
	int cks = f->tcr & 3;
	if (cks == 3) {
		uint64_t step = (uint64_t)(m->ftci_hz[n] * 1000.0);
		uint64_t per = (uint64_t)(m->phi * 1000.0);
		if (!step)
			return;
		f->ext += step * (uint64_t)states;
		while (f->ext >= per) {
			f->ext -= per;
			frt_tick(m, n);
		}
		return;
	}
	f->pre += (uint32_t)states;
	while (f->pre >= (uint32_t)div[cks]) {
		f->pre -= (uint32_t)div[cks];
		frt_tick(m, n);
	}
}

/* -------------------------------------------------------- 8-bit timer */

static void
tmo_match(h8532 *m, int os)
{
	switch (os & 3) {
	case 1: set_tmo(m, 0); break;
	case 2: set_tmo(m, 1); break;
	case 3: set_tmo(m, !m->tmo); break;
	}
}

static void
tmr_tick(h8532 *m)
{
	int clr = 0, a = 0, b = 0;
	if (m->t_tcnt == m->t_tcora) {
		m->t_tcsr |= 0x40;
		a = 1;
		if ((m->t_tcr & 0x18) == 0x08)
			clr = 1;
	}
	if (m->t_tcnt == m->t_tcorb) {
		m->t_tcsr |= 0x80;
		b = 1;
		if ((m->t_tcr & 0x18) == 0x10)
			clr = 1;
	}
	/* simultaneous: toggle > 1 > 0 */
	if (a && b) {
		int oa = m->t_tcsr & 3, ob = (m->t_tcsr >> 2) & 3;
		tmo_match(m, oa > ob ? oa : ob);
	} else if (a)
		tmo_match(m, m->t_tcsr & 3);
	else if (b)
		tmo_match(m, (m->t_tcsr >> 2) & 3);
	if (clr) {
		m->t_tcnt = 0;
		return;
	}
	if (++m->t_tcnt == 0)
		m->t_tcsr |= 0x20;
}

static void
tmr_run(h8532 *m, int states)
{
	static const int div[4] = { 0, 8, 64, 1024 };
	int cks = m->t_tcr & 7;
	if (cks == 0 || cks == 4)
		return;
	if (cks >= 5) {
		/* external TMCI: rising, falling or both edges */
		uint64_t step = (uint64_t)(m->tmci_hz * 1000.0) * (cks == 7 ? 2 : 1);
		uint64_t per = (uint64_t)(m->phi * 1000.0);
		if (!step)
			return;
		m->t_ext += step * (uint64_t)states;
		while (m->t_ext >= per) {
			m->t_ext -= per;
			tmr_tick(m);
		}
		return;
	}
	m->t_pre += (uint32_t)states;
	while (m->t_pre >= (uint32_t)div[cks]) {
		m->t_pre -= (uint32_t)div[cks];
		tmr_tick(m);
	}
}

/* ------------------------------------------------------------------ SCI */

static uint32_t
sci_bit_states(const h8532 *m)
{
	return 32u * (1u << (2 * (m->smr & 3))) * ((uint32_t)m->brr + 1);
}

double
h8532_sci_baud(const h8532 *m)
{
	return m->phi / sci_bit_states(m);
}

static uint32_t
sci_frame_states(const h8532 *m)
{
	int bits = 1 + ((m->smr & 0x40) ? 7 : 8) + ((m->smr & 0x20) ? 1 : 0)
	    + ((m->smr & 0x08) ? 2 : 1);
	return sci_bit_states(m) * (uint32_t)bits;
}

static void
sci_run(h8532 *m, int states)
{
	if (!(m->scr & S_TE))
		return;
	if (m->tx_busy) {
		if (m->tx_left > (uint32_t)states) {
			m->tx_left -= (uint32_t)states;
			return;
		}
		m->tx_busy = 0;
		if (m->bus.sci_tx)
			m->bus.sci_tx(m->bus.ctx, m->tsr);
	}
	if (!(m->ssr & S_TDRE)) {
		m->tsr = m->tdr;
		m->ssr |= S_TDRE;
		m->tx_busy = 1;
		m->tx_left = sci_frame_states(m);
	}
}

void
h8532_sci_rx(h8532 *m, uint8_t byte)
{
	if (!(m->scr & S_RE))
		return;
	if (m->ssr & S_RDRF) {
		m->ssr |= S_ORER;
		return;
	}
	if (m->smr & 0x40)
		byte &= 0x7F;
	m->rdr = byte;
	m->ssr |= S_RDRF;
}

/* ------------------------------------------------------------------ A/D */

static int
ad_first(const h8532 *m)
{
	return (m->adcsr & 4) ? 4 : 0;
}

static void
ad_start(h8532 *m)
{
	m->ad_ch = (m->adcsr & A_SCAN) ? ad_first(m) : (m->adcsr & 7);
	m->ad_left = (m->adcsr & A_CKS) ? 138 : 274;
}

static void
ad_run(h8532 *m, int states)
{
	int v, last;
	if (m->ad_ch < 0)
		return;
	if (m->ad_left > (uint32_t)states) {
		m->ad_left -= (uint32_t)states;
		return;
	}
	v = m->bus.adc ? m->bus.adc(m->bus.ctx, m->ad_ch) : 0;
	if (v < 0)
		v = 0;
	if (v > 1023)
		v = 1023;
	m->addr_[m->ad_ch & 3] = (uint16_t)(v << 6);
	if (!(m->adcsr & A_SCAN)) {
		m->adcsr = (uint8_t)((m->adcsr | A_ADF) & ~A_ADST);
		m->ad_ch = -1;
		return;
	}
	last = ad_first(m) + (m->adcsr & 3);
	if (m->ad_ch >= last) {
		m->adcsr |= A_ADF;
		m->ad_ch = ad_first(m);
	} else
		m->ad_ch++;
	m->ad_left = (m->adcsr & A_CKS) ? 128 : 256;
}

/* ------------------------------------------------------------------ WDT */

static void
wdt_run(h8532 *m, int states)
{
	static const uint32_t div[8] = { 2, 32, 64, 128, 256, 512, 2048, 4096 };
	uint32_t d;
	if (!(m->w_tcsr & W_TME))
		return;
	d = div[m->w_tcsr & 7];
	m->w_pre += (uint32_t)states;
	while (m->w_pre >= d) {
		m->w_pre -= d;
		if (++m->w_tcnt == 0) {
			m->w_tcsr |= W_OVF;
			if (m->w_tcsr & W_WT)
				m->nmi_req = 1;
			else
				m->wdt_irq0 = 1;
		}
	}
}

/* ----------------------------------------------------------- interrupts */

static void
irq_update(h8532 *m)
{
	int best_v = 0, best_l = 0, k;
	struct { int v, l, on; } s[24];
	int n = 0;

#define SRC(vec, lvl, cond) do { s[n].v = (vec); s[n].l = (lvl); s[n].on = (cond); n++; } while (0)
	SRC(V_IRQ0, (m->ipr[0] >> 4) & 7,
	    ((m->p1cr & 0x20) && !(m->pin[1] & 0x20)) || m->wdt_irq0);
	SRC(V_IRQ1, m->ipr[0] & 7, m->irq1_req);
	for (k = 0; k < 3; k++) {
		h8532_frt *f = &m->frt[k];
		int l = k == 0 ? (m->ipr[1] >> 4) & 7 : k == 1 ? m->ipr[1] & 7 : (m->ipr[2] >> 4) & 7;
		SRC(V_FRT + 4 * k, l, (f->tcsr & F_ICF) && (f->tcr & F_ICIE));
		SRC(V_FRT + 4 * k + 1, l, (f->tcsr & F_OCFA) && (f->tcr & F_OCIEA));
		SRC(V_FRT + 4 * k + 2, l, (f->tcsr & F_OCFB) && (f->tcr & F_OCIEB));
		SRC(V_FRT + 4 * k + 3, l, (f->tcsr & F_OVF) && (f->tcr & F_OVIE));
	}
	SRC(V_CMIA, m->ipr[2] & 7, (m->t_tcsr & 0x40) && (m->t_tcr & 0x40));
	SRC(V_CMIB, m->ipr[2] & 7, (m->t_tcsr & 0x80) && (m->t_tcr & 0x80));
	SRC(V_OVI, m->ipr[2] & 7, (m->t_tcsr & 0x20) && (m->t_tcr & 0x20));
	SRC(V_ERI, (m->ipr[3] >> 4) & 7, (m->ssr & (S_ORER | S_FER | S_PER)) && (m->scr & S_RIE));
	SRC(V_RXI, (m->ipr[3] >> 4) & 7, (m->ssr & S_RDRF) && (m->scr & S_RIE));
	SRC(V_TXI, (m->ipr[3] >> 4) & 7, (m->ssr & S_TDRE) && (m->scr & S_TIE));
	SRC(V_ADI, m->ipr[3] & 7, (m->adcsr & A_ADF) && (m->adcsr & A_ADIE));
#undef SRC

	if (m->nmi_req) {
		h8500_irq(&m->cpu, H8_VEC_NMI, 8);
		return;
	}
	/* vectors are in priority order, so the first of the highest level wins */
	for (k = 0; k < n; k++)
		if (s[k].on && s[k].l > best_l) {
			best_l = s[k].l;
			best_v = s[k].v;
		}
	h8500_irq(&m->cpu, best_v, best_l);
}

/* -------------------------------------------------------- register field */

static uint8_t
reg_read(h8532 *m, uint32_t a, int peek)
{
	int r = (int)(a & 0xFF);
	h8532_frt *f;

	if (r >= 0x90 && r < 0xC0 && (r & 0x0F) < 0x0A) {
		f = &m->frt[(r - 0x90) >> 4];
		switch (r & 0x0F) {
		case 0: return f->tcr;
		case 1: return f->tcsr;
		case 2: if (!peek) m->temp = (uint8_t)f->frc; return (uint8_t)(f->frc >> 8);
		case 3: return peek ? (uint8_t)f->frc : m->temp;
		case 4: return (uint8_t)(f->ocra >> 8);
		case 5: return (uint8_t)f->ocra;
		case 6: return (uint8_t)(f->ocrb >> 8);
		case 7: return (uint8_t)f->ocrb;
		case 8: if (!peek) m->temp = (uint8_t)f->icr; return (uint8_t)(f->icr >> 8);
		case 9: return peek ? (uint8_t)f->icr : m->temp;
		}
	}
	if (r >= 0xE0 && r <= 0xE7) {
		uint16_t v = m->addr_[(r - 0xE0) >> 1];
		if (!(r & 1)) {
			if (!peek)
				m->ad_temp = (uint8_t)v;
			return (uint8_t)(v >> 8);
		}
		return peek ? (uint8_t)v : m->ad_temp;
	}
	switch (r) {
	case 0x80: case 0x81: case 0x84: case 0x85: case 0x88: case 0x89:
	case 0x8C: case 0xFE:
		return 0xFF;	/* DDRs are write-only */
	case 0x82: return port_level(m, 1);
	case 0x8E: return port_level(m, 7);
	case 0x8F:
		return (uint8_t)(m->pin[8] | (m->ad_ch >= 0 ? 1 << m->ad_ch : 0));
	case 0xFF: return port_level(m, 9);
	case 0x83: case 0x86: case 0x87: case 0x8A: case 0x8B:
		return 0xFF;	/* bus ports in mode 3 */
	case 0xC0: case 0xC1: case 0xC2: return m->pwm[0][r - 0xC0];
	case 0xC4: case 0xC5: case 0xC6: return m->pwm[1][r - 0xC4];
	case 0xC8: case 0xC9: case 0xCA: return m->pwm[2][r - 0xC8];
	case 0xD0: return m->t_tcr;
	case 0xD1: return m->t_tcsr | 0x10;
	case 0xD2: return m->t_tcora;
	case 0xD3: return m->t_tcorb;
	case 0xD4: return m->t_tcnt;
	case 0xD8: return m->smr | 0x04;
	case 0xD9: return m->brr;
	case 0xDA: return m->scr | 0x0C;
	case 0xDB: return m->tdr;
	case 0xDC: return m->ssr | 0x07;
	case 0xDD: return m->rdr;
	case 0xE8: return m->adcsr;
	case 0xEC: return m->w_tcsr | 0x18;
	case 0xED: return m->w_tcnt;
	case 0xF0: case 0xF1: case 0xF2: case 0xF3: return m->ipr[r - 0xF0] & 0x77;
	case 0xF4: case 0xF5: case 0xF6: case 0xF7: return m->dte[r - 0xF4];
	case 0xF8: return m->wcr | 0xF0;
	case 0xF9: return m->ramcr | 0x7F;
	case 0xFA: return 0xC3;
	case 0xFB: return m->sbycr | 0x7F;
	case 0xFC: return m->p1cr | 0x87;
	}
	return 0xFF;
}

/* flags: writing 0 clears, writing 1 keeps */
static uint8_t
flags_w(uint8_t old, uint8_t v, uint8_t mask)
{
	return (uint8_t)((v & ~mask) | (old & v & mask));
}

static void
reg_write(h8532 *m, uint32_t a, uint8_t v)
{
	int r = (int)(a & 0xFF);
	h8532_frt *f;

	if (r >= 0x90 && r < 0xC0 && (r & 0x0F) < 0x0A) {
		f = &m->frt[(r - 0x90) >> 4];
		switch (r & 0x0F) {
		case 0: f->tcr = v; return;
		case 1: f->tcsr = flags_w(f->tcsr, v, 0xF0); return;
		case 2: case 4: case 6: m->temp = v; return;
		case 3: f->frc = (uint16_t)(m->temp << 8 | v); return;
		case 5: f->ocra = (uint16_t)(m->temp << 8 | v); return;
		case 7: f->ocrb = (uint16_t)(m->temp << 8 | v); return;
		}
		return;
	}
	switch (r) {
	case 0x80: m->ddr[1] = v; port_update(m, 1); return;
	case 0x82: m->dr[1] = v; port_update(m, 1); return;
	case 0x8C: m->ddr[7] = v; port_update(m, 7); return;
	case 0x8E: m->dr[7] = v; port_update(m, 7); return;
	case 0xFE: m->ddr[9] = v; port_update(m, 9); return;
	case 0xFF: m->dr[9] = v; port_update(m, 9); return;
	case 0xC0: case 0xC1: case 0xC2: m->pwm[0][r - 0xC0] = v; return;
	case 0xC4: case 0xC5: case 0xC6: m->pwm[1][r - 0xC4] = v; return;
	case 0xC8: case 0xC9: case 0xCA: m->pwm[2][r - 0xC8] = v; return;
	case 0xD0: m->t_tcr = v; return;
	case 0xD1:
		m->t_tcsr = (uint8_t)(flags_w(m->t_tcsr, v, 0xE0) & 0xEF);
		port_update(m, 1);
		return;
	case 0xD2: m->t_tcora = v; return;
	case 0xD3: m->t_tcorb = v; return;
	case 0xD4: m->t_tcnt = v; return;
	case 0xD8: m->smr = v; return;
	case 0xD9: m->brr = v; return;
	case 0xDA:
		if ((m->scr & S_TE) && !(v & S_TE)) {
			m->ssr |= S_TDRE;
			m->tx_busy = 0;
		}
		m->scr = v;
		return;
	case 0xDB: m->tdr = v; return;
	case 0xDC: m->ssr = flags_w(m->ssr, v, 0xF8); return;
	case 0xE8: {
		int start = (v & A_ADST) && !(m->adcsr & A_ADST);
		m->adcsr = (uint8_t)(flags_w(m->adcsr, v, A_ADF));
		if (!(v & A_ADST))
			m->ad_ch = -1;
		else if (start)
			ad_start(m);
		return;
	}
	case 0xEC: m->w_pw = v; return;
	case 0xED:
		if (m->w_pw == 0xA5) {
			m->w_tcsr = (uint8_t)(flags_w(m->w_tcsr, v, W_OVF) & 0xE7);
			if (!(m->w_tcsr & W_TME))
				m->w_tcnt = 0;
		} else if (m->w_pw == 0x5A)
			m->w_tcnt = v;
		m->w_pw = 0;
		return;
	case 0xF0: case 0xF1: case 0xF2: case 0xF3: m->ipr[r - 0xF0] = v & 0x77; return;
	case 0xF4: case 0xF5: case 0xF6: case 0xF7: m->dte[r - 0xF4] = v; return;
	case 0xF8: m->wcr = v & 0x0F; return;
	case 0xF9: m->ramcr = v & 0x80; return;
	case 0xFB: m->sbycr = v & 0x80; return;
	case 0xFC: m->p1cr = v & 0x78; return;
	}
}

/* ------------------------------------------------------------------ bus */

static uint8_t
bus_read(void *ctx, uint32_t a)
{
	h8532 *m = ctx;
	if (a >= 0xFF80 && a <= 0xFFFF)
		return reg_read(m, a, 0);
	if (in_iram(m, a))
		return m->iram[a - 0xFB80];
	return m->bus.read(m->bus.ctx, a);
}

static void
bus_write(void *ctx, uint32_t a, uint8_t v)
{
	h8532 *m = ctx;
	if (a >= 0xFF80 && a <= 0xFFFF) {
		reg_write(m, a, v);
		return;
	}
	if (in_iram(m, a)) {
		m->iram[a - 0xFB80] = v;
		return;
	}
	m->bus.write(m->bus.ctx, a, v);
}

static int
bus_states(void *ctx, uint32_t a)
{
	h8532 *m = ctx;
	if (a >= 0xFF80 && a <= 0xFFFF)
		return 3;
	if (in_iram(m, a))
		return 1;	/* 2 states per 16-bit access */
	if ((m->wcr & 0x0C) == 0x04)
		return 3;
	return 3 + (m->wcr & 3);
}

uint8_t
h8532_peek(h8532 *m, uint32_t a)
{
	a &= 0xFFFFFF;
	if (a >= 0xFF80 && a <= 0xFFFF)
		return reg_read(m, a, 1);
	if (in_iram(m, a))
		return m->iram[a - 0xFB80];
	return m->bus.read(m->bus.ctx, a);
}

void
h8532_poke(h8532 *m, uint32_t a, uint8_t v)
{
	a &= 0xFFFFFF;
	if (in_iram(m, a))
		m->iram[a - 0xFB80] = v;
	else if (a < 0xFF80 || a > 0xFFFF)
		m->bus.write(m->bus.ctx, a, v);
}

/* ---------------------------------------------------------------- pins */

void
h8532_pin(h8532 *m, int port, int bit, int level)
{
	uint8_t mask = (uint8_t)(1 << bit), old = m->pin[port] & mask;
	int k;
	if (level)
		m->pin[port] |= mask;
	else
		m->pin[port] &= (uint8_t)~mask;
	if (old == (m->pin[port] & mask))
		return;
	if (port == 1 && bit == 6 && !level && (m->p1cr & 0x40))
		m->irq1_req = 1;	/* IRQ1 falling edge */
	if (port == 7)
		for (k = 0; k < 3; k++)	/* FTI1..3 on P71..P73 */
			if (bit == k + 1 && !(m->ddr[7] & mask)
			    && (!!(m->frt[k].tcsr & F_IEDG)) == !!level)
				frt_capture(m, k);
	if (!(m->ddr[port] & mask))
		port_update(m, port);
}

void
h8532_nmi_pin(h8532 *m, int level)
{
	int rising = (m->p1cr & 0x10) != 0;
	level = !!level;
	if (level != m->nmi_level && (level ? rising : !rising))
		m->nmi_req = 1;
	m->nmi_level = (uint8_t)level;
}

/* ---------------------------------------------------------------- core */

void
h8532_init(h8532 *m, const h8532_bus *bus, double phi)
{
	h8500_bus cb = { m, bus_read, bus_write, bus_states };
	memset(m, 0, sizeof(*m));
	m->bus = *bus;
	m->phi = phi;
	h8500_init(&m->cpu, &cb);
	memset(m->pin, 0xFF, sizeof(m->pin));
	m->nmi_level = 1;
}

void
h8532_reset(h8532 *m)
{
	int k;
	memset(m->ddr, 0, sizeof(m->ddr));
	memset(m->dr, 0, sizeof(m->dr));
	memset(m->last_out, 0xAA, sizeof(m->last_out));
	m->ddr[1] = 0x03;	/* P10 φ, P11 E */
	m->p1cr = 0;
	m->wcr = 0x03;
	m->ramcr = 0x80;
	m->sbycr = 0;
	memset(m->ipr, 0, sizeof(m->ipr));
	memset(m->dte, 0, sizeof(m->dte));
	for (k = 0; k < 3; k++) {
		memset(&m->frt[k], 0, sizeof(m->frt[k]));
		m->frt[k].ocra = m->frt[k].ocrb = 0xFFFF;
	}
	m->t_tcr = 0;
	m->t_tcsr = 0;
	m->t_tcora = m->t_tcorb = 0xFF;
	m->t_tcnt = 0;
	m->tmo = 0;
	memset(m->pwm, 0, sizeof(m->pwm));
	for (k = 0; k < 3; k++) {
		m->pwm[k][0] = 0x38;
		m->pwm[k][1] = 0xFF;
	}
	m->smr = 0;
	m->brr = 0xFF;
	m->scr = 0;
	m->tdr = 0xFF;
	m->ssr = S_TDRE;
	m->rdr = 0;
	m->tx_busy = 0;
	m->adcsr = 0;
	memset(m->addr_, 0, sizeof(m->addr_));
	m->ad_ch = -1;
	m->w_tcsr = 0;
	m->w_tcnt = 0;
	m->w_pre = 0;
	m->nmi_req = m->irq1_req = m->wdt_irq0 = 0;
	h8500_reset(&m->cpu);
	m->cpu.irq_inhibit = 1;		/* nothing before the first instruction */
	port_update(m, 1);
	port_update(m, 7);
	port_update(m, 9);
}

int
h8532_step(h8532 *m)
{
	int st, k;

	irq_update(m);
	st = h8500_step(&m->cpu);
	switch (m->cpu.last_exc) {	/* accepted: drop the edge requests */
	case H8_VEC_NMI: m->nmi_req = 0; break;
	case V_IRQ0: m->wdt_irq0 = 0; break;
	case V_IRQ1: m->irq1_req = 0; break;
	}
	for (k = 0; k < 3; k++)
		frt_run(m, k, st);
	tmr_run(m, st);
	sci_run(m, st);
	ad_run(m, st);
	wdt_run(m, st);
	m->states += (uint64_t)st;
	return st;
}
