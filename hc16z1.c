/*
 * MC68HC16Z1 modules; see hc16z1.h.
 */
#include <string.h>

#include "hc16z1.h"

#define REG(a)	(((a) & 0xFF000) == 0xFF000)	/* module registers, MM = 1 */

/* ---------------------------------------------------------------- clock */

/* Fsys = Fref * 4 * (Y + 1) * 2^(2W + X), SYNCR W = bit 15, X = bit 14,
 * Y = bits 13..8 */
static uint32_t
fsys(const hc16z1 *m)
{
	unsigned w = m->syncr >> 15 & 1, x = m->syncr >> 14 & 1, y = m->syncr >> 8 & 0x3F;
	return (uint32_t)HC16_EXTAL * 4 * (y + 1) << (2 * w + x);
}

double
hc16z1_fsys(const hc16z1 *m)
{
	return fsys(m);
}

/* ---------------------------------------------------------------- chip selects */

/* pin assignment field of chip select n (0..10, 11 = CSBOOT):
 * 00 discrete output/port, 01 alternate function, 10 8-bit CS, 11 16-bit CS */
static unsigned
cs_pa(const hc16z1 *m, int n)
{
	static const struct { uint8_t reg, shift; } f[12] = {
		{ 0, 2 }, { 0, 4 }, { 0, 6 },		/* CS0..CS2 */
		{ 0, 8 }, { 0, 10 }, { 0, 12 },		/* CS3..CS5 */
		{ 1, 0 }, { 1, 2 }, { 1, 4 }, { 1, 6 },	/* CS6..CS9 */
		{ 1, 8 },				/* CS10 */
		{ 0, 0 },				/* CSBOOT */
	};
	return m->cspar[f[n].reg] >> f[n].shift & 3;
}

/* block sizes of BLKSZ 0..7 (the 1M encoding compares ADDR[23:20],
 * which on the CPU16 follow ADDR19) */
static const uint32_t blk[8] = {
	0x800, 0x2000, 0x4000, 0x10000, 0x20000, 0x40000, 0x80000, 0x100000
};

/* chip selects asserted by a normal (supervisor, non-IACK) access to the
 * 20-bit address a: write, and for 16-bit ports which byte lanes */
static unsigned
cs_match(const hc16z1 *m, uint32_t a, int write, int upper, int lower, int *port8, int *clocks)
{
	unsigned mask = 0;
	uint32_t a24 = a | ((a & 0x80000) ? 0xF00000 : 0);
	*port8 = 0;
	*clocks = 0;
	for (int n = 0; n < 12; n++) {
		unsigned pa = cs_pa(m, n), opt = m->csor[n], rw, byte, space, dsack;
		uint32_t base, size;
		if (pa < 2)
			continue;
		size = blk[m->csbar[n] & 7];
		base = (uint32_t)(m->csbar[n] & 0xFFF8) << 8;
		if ((a24 & ~(size - 1) & 0xFFFFFF) != (base & ~(size - 1)))
			continue;
		space = opt >> 4 & 3;
		if (space < 2)			/* CPU space / user only */
			continue;
		rw = opt >> 11 & 3;
		if (!(rw & (write ? 2 : 1)))
			continue;
		byte = opt >> 13 & 3;
		if (pa == 3) {			/* BYTE matters for 16-bit ports only */
			if (byte == 0)
				continue;
			if (!(((byte & 2) && upper) || ((byte & 1) && lower)))
				continue;
		}
		mask |= n == 11 ? CS_BOOT : 1u << n;
		if (pa == 2)
			*port8 = 1;
		if (!*clocks) {
			dsack = opt >> 6 & 0xF;
			*clocks = dsack == 0xE ? 2 : dsack == 0xF ? 3 : 3 + (int)dsack;
		}
	}
	return mask;
}

/* ---------------------------------------------------------------- forward */

static uint16_t reg_read(hc16z1 *m, uint32_t a, int size);
static void     reg_write(hc16z1 *m, uint32_t a, int size, uint16_t v);

static int
in_ram(const hc16z1 *m, uint32_t a)
{
	uint32_t base;
	if (m->rammcr & 0x8000)		/* STOP */
		return 0;
	base = ((uint32_t)(m->rambah & 0xFF) << 16 | (m->rambal & 0xFC00)) & 0xFFFFF;
	return (a & ~0x3FFu) == base;
}

/* ---------------------------------------------------------------- bus */

static uint16_t
ext_read(hc16z1 *m, uint32_t a, int size)
{
	int port8, clocks;
	unsigned cs = cs_match(m, a, 0, size == 2 || !(a & 1), size == 2 || (a & 1), &port8, &clocks);
	if (!cs) {
		m->unmapped_addr = a;
		m->unmapped_count++;
		return size == 2 ? 0xFFFF : 0xFF;
	}
	if (port8 && size == 2) {
		uint16_t hi = m->board.ext_read(m->board.ctx, a, 1, cs);
		unsigned cs2 = cs_match(m, a + 1, 0, 0, 1, &port8, &clocks);
		return (uint16_t)(hi << 8 | m->board.ext_read(m->board.ctx, a + 1, 1, cs2));
	}
	return m->board.ext_read(m->board.ctx, a, size, cs);
}

static void
ext_write(hc16z1 *m, uint32_t a, int size, uint16_t v)
{
	int port8, clocks;
	unsigned cs = cs_match(m, a, 1, size == 2 || !(a & 1), size == 2 || (a & 1), &port8, &clocks);
	if (!cs) {
		m->unmapped_addr = a;
		m->unmapped_count++;
		return;
	}
	if (port8 && size == 2) {
		m->board.ext_write(m->board.ctx, a, 1, v >> 8, cs);
		cs = cs_match(m, a + 1, 1, 0, 1, &port8, &clocks);
		m->board.ext_write(m->board.ctx, a + 1, 1, v & 0xFF, cs);
		return;
	}
	m->board.ext_write(m->board.ctx, a, size, v, cs);
}

static uint8_t
bus_rd8(void *ctx, uint32_t a)
{
	hc16z1 *m = ctx;
	if (REG(a))
		return (uint8_t)reg_read(m, a, 1);
	if (in_ram(m, a))
		return m->ram[a & 0x3FF];
	return (uint8_t)ext_read(m, a, 1);
}

static void
bus_wr8(void *ctx, uint32_t a, uint8_t v)
{
	hc16z1 *m = ctx;
	if (REG(a)) {
		reg_write(m, a, 1, v);
		return;
	}
	if (in_ram(m, a)) {
		m->ram[a & 0x3FF] = v;
		return;
	}
	ext_write(m, a, 1, v);
}

static uint16_t
bus_rd16(void *ctx, uint32_t a)
{
	hc16z1 *m = ctx;
	if (REG(a))
		return reg_read(m, a, 2);
	if (in_ram(m, a))
		return (uint16_t)(m->ram[a & 0x3FF] << 8 | m->ram[(a + 1) & 0x3FF]);
	return ext_read(m, a, 2);
}

static void
bus_wr16(void *ctx, uint32_t a, uint16_t v)
{
	hc16z1 *m = ctx;
	if (REG(a)) {
		reg_write(m, a, 2, v);
		return;
	}
	if (in_ram(m, a)) {
		m->ram[a & 0x3FF] = (uint8_t)(v >> 8);
		m->ram[(a + 1) & 0x3FF] = (uint8_t)v;
		return;
	}
	ext_write(m, a, 2, v);
}

/* internal modules and the standby RAM take two clocks; external cycles
 * three plus wait states (two with fast termination), twice that for a
 * word on an 8-bit port */
static int
bus_clocks(void *ctx, uint32_t a, int size)
{
	hc16z1 *m = ctx;
	int port8, clocks;
	if (REG(a) || in_ram(m, a))
		return 2;
	if (!cs_match(m, a, 0, size == 2 || !(a & 1), size == 2 || (a & 1), &port8, &clocks)
	    && !cs_match(m, a, 1, size == 2 || !(a & 1), size == 2 || (a & 1), &port8, &clocks))
		return 3;
	return port8 && size == 2 ? 2 * clocks : clocks;
}

/* ---------------------------------------------------------------- interrupts */

/* request level and vector of each source; arbitration among equal
 * levels by the modules' IARB (higher wins) */
enum { SRC_PIT, SRC_GPT, SRC_SCI, SRC_N };

static int  gpt_request(hc16z1 *m, int *vec);
static int  sci_request(hc16z1 *m, int *vec);

static int
pit_request(hc16z1 *m, int *vec)
{
	int lvl = m->picr >> 8 & 7;
	if (!m->pit_pending || !lvl)
		return 0;
	*vec = m->picr & 0xFF;
	return lvl;
}

static int
request(hc16z1 *m, int src, int *vec, int *iarb)
{
	switch (src) {
	case SRC_PIT: *iarb = m->simcr & 0xF; return pit_request(m, vec);
	case SRC_GPT: *iarb = m->gptmcr & 0xF; return gpt_request(m, vec);
	case SRC_SCI: *iarb = m->qsmcr & 0xF; return sci_request(m, vec);
	}
	return 0;
}

static void
update_irq(hc16z1 *m)
{
	int best = 0, vec, iarb;
	for (int s = 0; s < SRC_N; s++) {
		int l = request(m, s, &vec, &iarb);
		if (l > best && iarb)
			best = l;
	}
	m->cpu.irq_level = best;
}

static void gpt_ack(hc16z1 *m, int vec);

static int
bus_iack(void *ctx, int level)
{
	hc16z1 *m = ctx;
	int win = -1, wvec = C16_VEC_SPURIOUS, warb = -1, vec, iarb;
	for (int s = 0; s < SRC_N; s++) {
		int l = request(m, s, &vec, &iarb);
		if (l == level && iarb && iarb > warb) {
			win = s;
			wvec = vec;
			warb = iarb;
		}
	}
	if (win == SRC_PIT)
		m->pit_pending = 0;
	else if (win == SRC_GPT)
		gpt_ack(m, wvec);
	update_irq(m);
	return wvec;
}

/* ---------------------------------------------------------------- SIM */

static void
pins(hc16z1 *m, int port)
{
	switch (port) {
	case HC16_PORTC:
		m->board.pins_out(m->board.ctx, HC16_PORTC, m->portc, 0x7F);
		break;
	case HC16_PORTE:
		m->board.pins_out(m->board.ctx, HC16_PORTE, m->porte, m->ddre & ~m->pepar);
		break;
	case HC16_PORTF:
		m->board.pins_out(m->board.ctx, HC16_PORTF, m->portf, m->ddrf & ~m->pfpar);
		break;
	case HC16_PORTGP:
		m->board.pins_out(m->board.ctx, HC16_PORTGP, m->portgp, m->ddrgp);
		break;
	case HC16_PORTQS:
		m->board.pins_out(m->board.ctx, HC16_PORTQS, m->portqs, m->ddrqs & ~m->pqspar);
		break;
	}
}

/* a port data register reads its output latch on output pins and the
 * pin level on inputs */
static uint8_t
port_read(hc16z1 *m, int port, uint8_t latch, uint8_t ddr)
{
	uint8_t in = m->board.pins_in(m->board.ctx, port);
	return (uint8_t)((latch & ddr) | (in & ~ddr));
}

static uint32_t
wd_ratio(const hc16z1 *m)
{
	static const uint8_t sh[8] = { 9, 11, 13, 15, 18, 20, 22, 24 };
	return 1u << sh[(m->sypcr >> 4) & 7];
}

static uint16_t
sim_read(hc16z1 *m, uint32_t a)
{
	switch (a & 0xFE) {
	case 0x00: return m->simcr;
	case 0x04: return m->syncr | 0x0008;	/* SLOCK: always locked */
	case 0x06: return m->rsr;
	case 0x10: case 0x12: return port_read(m, HC16_PORTE, m->porte, m->ddre);
	case 0x14: return m->ddre;
	case 0x16: return m->pepar;
	case 0x18: case 0x1A: return port_read(m, HC16_PORTF, m->portf, m->ddrf);
	case 0x1C: return m->ddrf;
	case 0x1E: return m->pfpar;
	case 0x20: return m->sypcr;
	case 0x22: return m->picr;
	case 0x24: return m->pitr;
	case 0x26: return 0;
	case 0x40: return m->portc;
	case 0x44: return m->cspar[0];
	case 0x46: return m->cspar[1];
	}
	if ((a & 0xFF) >= 0x48 && (a & 0xFF) <= 0x77) {
		int i = ((int)(a & 0xFF) - 0x48) / 4, opt = (a & 2) != 0;
		int n = i == 0 ? 11 : i - 1;
		return opt ? m->csor[n] : m->csbar[n];
	}
	return 0;
}

/* bytes: 2 = upper (even address), 1 = lower */
static void
sim_write(hc16z1 *m, uint32_t a, uint16_t v, int bytes)
{
	uint8_t lo = (uint8_t)v;
	switch (a & 0xFE) {
	case 0x00:
		if (bytes & 2)
			m->simcr = (uint16_t)((m->simcr & 0x00FF) | (v & 0xFF00) | 0x0040);	/* MM stays 1 */
		if (bytes & 1)
			m->simcr = (uint16_t)((m->simcr & 0xFF00) | (lo & 0xCF) | 0x40);
		return;
	case 0x04:
		if (bytes & 2)
			m->syncr = (uint16_t)((m->syncr & 0x00FF) | (v & 0xFF00));
		if (bytes & 1)
			m->syncr = (uint16_t)((m->syncr & 0xFF00) | (lo & 0x93));
		return;
	case 0x10: case 0x12: if (bytes & 1) { m->porte = lo; pins(m, HC16_PORTE); } return;
	case 0x14: if (bytes & 1) { m->ddre = lo; pins(m, HC16_PORTE); } return;
	case 0x16: if (bytes & 1) { m->pepar = lo; pins(m, HC16_PORTE); } return;
	case 0x18: case 0x1A: if (bytes & 1) { m->portf = lo; pins(m, HC16_PORTF); } return;
	case 0x1C: if (bytes & 1) { m->ddrf = lo; pins(m, HC16_PORTF); } return;
	case 0x1E: if (bytes & 1) { m->pfpar = lo; pins(m, HC16_PORTF); } return;
	case 0x20:			/* SYPCR: write once after reset */
		if ((bytes & 1) && !m->sypcr_written) {
			m->sypcr = lo;
			m->sypcr_written = 1;
		}
		return;
	case 0x22:
		if (bytes & 2) m->picr = (uint16_t)((m->picr & 0x00FF) | (v & 0x0700));
		if (bytes & 1) m->picr = (uint16_t)((m->picr & 0xFF00) | lo);
		update_irq(m);
		return;
	case 0x24:
		if (bytes & 2) m->pitr = (uint16_t)((m->pitr & 0x00FF) | (v & 0x0100));
		if (bytes & 1) m->pitr = (uint16_t)((m->pitr & 0xFF00) | lo);
		return;
	case 0x26:			/* SWSR: $55 then $AA services the watchdog */
		if (bytes & 1) {
			if (lo == 0xAA && m->swsr_last == 0x55)
				m->wd_count = 0;
			m->swsr_last = lo;
		}
		return;
	case 0x40:
		if (bytes & 1) { m->portc = lo & 0x7F; pins(m, HC16_PORTC); }
		return;
	case 0x44: case 0x46: {
		uint16_t *p = &m->cspar[(a & 2) != 0];
		if (bytes & 2) *p = (uint16_t)((*p & 0x00FF) | (v & 0xFF00));
		if (bytes & 1) *p = (uint16_t)((*p & 0xFF00) | lo);
		return;
	}
	}
	if ((a & 0xFF) >= 0x48 && (a & 0xFF) <= 0x77) {
		int i = ((int)(a & 0xFF) - 0x48) / 4, opt = (a & 2) != 0;
		int n = i == 0 ? 11 : i - 1;
		uint16_t *p = opt ? &m->csor[n] : &m->csbar[n];
		if (bytes & 2) *p = (uint16_t)((*p & 0x00FF) | (v & 0xFF00));
		if (bytes & 1) *p = (uint16_t)((*p & 0xFF00) | lo);
	}
}

static uint16_t
sram_read(hc16z1 *m, uint32_t a)
{
	switch (a & 0xFE) {
	case 0x00: return m->rammcr;
	case 0x04: return m->rambah;
	case 0x06: return m->rambal;
	}
	return 0;
}

/* the base registers are writable only while RLCK = 0 */
static void
sram_write(hc16z1 *m, uint32_t a, uint16_t v, int bytes)
{
	uint16_t *p;
	switch (a & 0xFE) {
	case 0x00: p = &m->rammcr; break;
	case 0x04: if (m->rammcr & 0x0800) return; p = &m->rambah; break;
	case 0x06: if (m->rammcr & 0x0800) return; p = &m->rambal; break;
	default: return;
	}
	if (p == &m->rammcr && (m->rammcr & 0x0800))
		v |= 0x0800;		/* RLCK stays set */
	if (bytes & 2) *p = (uint16_t)((*p & 0x00FF) | (v & 0xFF00));
	if (bytes & 1) *p = (uint16_t)((*p & 0xFF00) | (v & 0x00FF));
}

/* ---------------------------------------------------------------- QSM: SCI */

#define SC_TDRE	0x0100
#define SC_TC	0x0080
#define SC_RDRF	0x0040
#define SC_RAF	0x0020
#define SC_IDLE	0x0010
#define SC_OR	0x0008

#define CR_TIE	0x0080
#define CR_TCIE	0x0040
#define CR_RIE	0x0020
#define CR_ILIE	0x0010
#define CR_TE	0x0008
#define CR_RE	0x0004
#define CR_M	0x0200
#define CR_PE	0x0400

double
hc16z1_sci_baud(const hc16z1 *m)
{
	unsigned br = m->sccr0 & 0x1FFF;
	return br ? (double)fsys(m) / (32.0 * br) : 0;
}

/* clocks of one frame: start, 8 or 9 data bits (parity included), stop */
static uint64_t
sci_frame(const hc16z1 *m)
{
	unsigned br = m->sccr0 & 0x1FFF, bits = (m->sccr1 & CR_M) ? 11 : 10;
	return (uint64_t)32 * (br ? br : 1) * bits;
}

static int
sci_request(hc16z1 *m, int *vec)
{
	uint16_t s = m->scsr, c = m->sccr1;
	int lvl = m->qilr & 7;
	if (!lvl)
		return 0;
	if (((c & CR_TIE) && (s & SC_TDRE)) || ((c & CR_TCIE) && (s & SC_TC))
	    || ((c & CR_RIE) && (s & (SC_RDRF | SC_OR))) || ((c & CR_ILIE) && (s & SC_IDLE))) {
		*vec = m->qivr & 0xFE;
		return lvl;
	}
	return 0;
}

static void
sci_start_tx(hc16z1 *m)
{
	if (m->tx_busy || (m->scsr & SC_TDRE) || !(m->sccr1 & CR_TE))
		return;
	m->tsr = m->tdr;
	m->scsr |= SC_TDRE;
	m->scsr &= ~SC_TC;
	m->tx_busy = 1;
	m->tx_done = m->clk + sci_frame(m);
}

static void
sci_tick(hc16z1 *m)
{
	if (m->tx_busy && m->clk >= m->tx_done) {
		m->tx_busy = 0;
		m->board.sci_tx(m->board.ctx, m->tsr);
		if (!(m->scsr & SC_TDRE))
			sci_start_tx(m);
		else
			m->scsr |= SC_TC;
	}
	if ((m->sccr1 & CR_RE) && m->rxq_head != m->rxq_tail) {
		if (!m->rx_next)
			m->rx_next = m->clk + sci_frame(m);
		if (m->clk >= m->rx_next) {
			uint16_t f = m->rxq[m->rxq_tail];
			m->rxq_tail = (m->rxq_tail + 1) % 512;
			if (m->scsr & SC_RDRF)
				m->scsr |= SC_OR;	/* the new frame is lost */
			else {
				m->rdr = f;
				m->scsr |= SC_RDRF;
			}
			m->rx_idle_armed = 1;
			m->rx_next = m->rxq_head != m->rxq_tail ? m->rx_next + sci_frame(m) : 0;
		}
	} else if (m->rx_idle_armed && !m->rx_next) {
		m->scsr |= SC_IDLE;		/* line idle after the last frame */
		m->rx_idle_armed = 0;
	}
}

void
hc16z1_sci_rx(hc16z1 *m, uint16_t frame)
{
	int next = (m->rxq_head + 1) % 512;
	if (next == m->rxq_tail)
		return;
	m->rxq[m->rxq_head] = frame;
	m->rxq_head = next;
}

int
hc16z1_sci_rx_pending(const hc16z1 *m)
{
	return (m->rxq_head - m->rxq_tail + 512) % 512;
}

static uint16_t
qsm_read(hc16z1 *m, uint32_t a, int bytes)
{
	switch (a & 0xFE) {
	case 0x00: return m->qsmcr;
	case 0x04: return (uint16_t)(m->qilr << 8 | m->qivr);
	case 0x08: return m->sccr0;
	case 0x0A: return m->sccr1;
	case 0x0C:
		if (bytes)
			m->scsr_read = 1;
		return m->scsr;
	case 0x0E:
		/* SCSR read then SCDR read clears the receive flags */
		if (!bytes)
			return m->rdr;
		if (m->scsr_read)
			m->scsr &= ~(SC_RDRF | SC_IDLE | SC_OR | 0x7);
		m->scsr_read = 0;
		return m->rdr & ((m->sccr1 & CR_M) ? 0x1FF : 0xFF);
	case 0x14: return (uint16_t)(port_read(m, HC16_PORTQS, m->portqs, m->ddrqs) | m->pqspar << 8);
	case 0x16: return (uint16_t)(m->pqspar << 8 | m->ddrqs);
	case 0x18: case 0x1A: case 0x1C: return m->spcr[(a & 6) >> 1 & 3];
	case 0x1E: return (uint16_t)(m->spcr[3] & 0xFF00) | m->spsr;
	}
	return 0;
}

static void
qsm_write(hc16z1 *m, uint32_t a, uint16_t v, int bytes)
{
	uint8_t lo = (uint8_t)v, hi = (uint8_t)(v >> 8);
	switch (a & 0xFE) {
	case 0x00:
		if (bytes & 2) m->qsmcr = (uint16_t)((m->qsmcr & 0x00FF) | (v & 0xFF00));
		if (bytes & 1) m->qsmcr = (uint16_t)((m->qsmcr & 0xFF00) | lo);
		return;
	case 0x04:
		if (bytes & 2) m->qilr = hi & 0x3F;
		if (bytes & 1) m->qivr = lo;
		return;
	case 0x08:
		if (bytes & 2) m->sccr0 = (uint16_t)((m->sccr0 & 0x00FF) | (v & 0x1F00));
		if (bytes & 1) m->sccr0 = (uint16_t)((m->sccr0 & 0xFF00) | lo);
		return;
	case 0x0A:
		if (bytes & 2) m->sccr1 = (uint16_t)((m->sccr1 & 0x00FF) | (v & 0x7F00));
		if (bytes & 1) m->sccr1 = (uint16_t)((m->sccr1 & 0xFF00) | lo);
		sci_start_tx(m);
		return;
	case 0x0C:
		return;
	case 0x0E:
		/* SCSR read then SCDR write clears TDRE and TC */
		if (m->scsr_read)
			m->scsr &= ~(SC_TDRE | SC_TC);
		m->scsr_read = 0;
		if (bytes & 2) m->tdr = (uint16_t)((m->tdr & 0x00FF) | (v & 0x0100));
		if (bytes & 1) m->tdr = (uint16_t)((m->tdr & 0xFF00) | lo);
		sci_start_tx(m);
		return;
	case 0x14:
		if (bytes & 1) { m->portqs = lo; pins(m, HC16_PORTQS); }
		return;
	case 0x16:
		if (bytes & 2) m->pqspar = hi & 0x7B;
		if (bytes & 1) m->ddrqs = lo;
		pins(m, HC16_PORTQS);
		return;
	case 0x18: case 0x1A: case 0x1C: case 0x1E: {
		uint16_t *p = &m->spcr[(a & 6) >> 1 & 3];
		if (bytes & 2) *p = (uint16_t)((*p & 0x00FF) | (v & 0xFF00));
		if (bytes & 1 && (a & 0xFE) != 0x1E) *p = (uint16_t)((*p & 0xFF00) | lo);
		return;
	}
	}
}

/* ---------------------------------------------------------------- GPT */

/* sources 1..11: IC1 IC2 IC3 OC1 OC2 OC3 OC4 IC4/OC5 TOF PAOV PAI;
 * flag and enable bits in TFLG1/TMSK1 (1..8), TFLG2/TMSK2 (9..11) */
static int
gpt_pending(const hc16z1 *m, int src)
{
	static const uint8_t bit1[9] = { 0, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 };
	static const uint8_t bit2[3] = { 0x80, 0x20, 0x10 };	/* TOF PAOVF PAIF */
	if (src >= 1 && src <= 8)
		return (m->tflg1 & m->tmsk1 & bit1[src]) != 0;
	if (src >= 9 && src <= 11)
		return (m->tflg2 & m->tmsk2 & bit2[src - 9]) != 0;
	return 0;
}

/* the pending source with the highest priority: the IPA source first
 * (vector low nibble 0), then IC1 (1) .. PAI (11) */
static int
gpt_request(hc16z1 *m, int *vec)
{
	int lvl = m->gpticr >> 8 & 7, ipa = m->gpticr >> 12 & 0xF, base = (m->gpticr >> 4 & 0xF) << 4;
	if (!lvl)
		return 0;
	if (ipa >= 1 && ipa <= 11 && gpt_pending(m, ipa)) {
		*vec = base;
		return lvl;
	}
	for (int s = 1; s <= 11; s++)
		if (gpt_pending(m, s)) {
			*vec = base | s;
			return lvl;
		}
	return 0;
}

/* the flags stay until the handler clears them */
static void
gpt_ack(hc16z1 *m, int vec)
{
	(void)m;
	(void)vec;
}

static int
oc_pin(int n)		/* OC1..OC5 on GP3..GP7 */
{
	return 1 << (n + 2);
}

static void
oc_action(hc16z1 *m, int n)
{
	uint8_t pin;
	if (n == 1) {
		uint8_t mask = m->oc1m & 0xF8;
		m->portgp = (uint8_t)((m->portgp & ~mask) | (m->oc1d & mask));
		pins(m, HC16_PORTGP);
		return;
	}
	switch (m->tctl1 >> (2 * (n - 2)) & 3) {
	case 0: return;
	case 1: pin = (uint8_t)(m->portgp ^ oc_pin(n)); break;
	case 2: pin = (uint8_t)(m->portgp & ~oc_pin(n)); break;
	default: pin = (uint8_t)(m->portgp | oc_pin(n)); break;
	}
	m->portgp = pin;
	pins(m, HC16_PORTGP);
}

/* clocks of one PWM counter count */
static unsigned
pwm_div(const hc16z1 *m)
{
	unsigned ppr = m->pwmc >> 4 & 7;
	return ppr == 7 ? 2 : 2u << ppr;
}

static void
pwm_report(hc16z1 *m, int ch, int duty)
{
	if (m->pwm_duty[ch] != duty) {
		m->pwm_duty[ch] = duty;
		m->board.pwm(m->board.ctx, ch, duty);
	}
}

/* advance TCNT and the PWM counter over n system clocks */
static void
gpt_tick(hc16z1 *m, int n)
{
	static const uint8_t cpr_div[8] = { 4, 8, 16, 32, 64, 128, 0, 0 };
	unsigned div = (m->tmsk2 & 7) == 6 ? 256 : cpr_div[m->tmsk2 & 7];
	if (m->gptmcr & 0x9000)		/* STOP or STOPP */
		return;
	m->prescl = (uint16_t)((m->prescl + n) & 0x1FF);
	if (div) {
		m->tcnt_acc += (uint32_t)n;
		while (m->tcnt_acc >= div) {
			m->tcnt_acc -= div;
			m->tcnt++;
			if (!m->tcnt)
				m->tflg2 |= 0x80;		/* TOF */
			for (int k = 0; k < 4; k++)
				if (m->tcnt == m->toc[k]) {
					m->tflg1 |= (uint8_t)(0x08 << k);	/* OC1F..OC4F */
					oc_action(m, k + 1);
				}
			if (!(m->pactl & 0x04) && m->tcnt == m->ti4o5) {
				m->tflg1 |= 0x80;
				oc_action(m, 5);
			}
		}
	}
	/* PWM: the duty reloads from PWMx at each period start */
	{
		unsigned pd = pwm_div(m);
		m->pwm_acc += (uint32_t)n;
		while (m->pwm_acc >= pd) {
			m->pwm_acc -= pd;
			m->pwmcnt++;
			if (!(m->pwmcnt & 0xFF)) {
				m->pwmbufa = m->pwma;
				m->pwmbufb = m->pwmb;
				pwm_report(m, 0, (m->pwmc & 0x02) ? 256 : m->pwmbufa);
				pwm_report(m, 1, (m->pwmc & 0x01) ? 256 : m->pwmbufb);
			}
		}
	}
}

/* input captures on GP0..GP2 (and GP7 as IC4) from the pin levels */
static void
gpt_inputs(hc16z1 *m)
{
	uint8_t in = m->board.pins_in(m->board.ctx, HC16_PORTGP), ch = (uint8_t)(in ^ m->gp_in_last);
	for (int k = 0; k < 4; k++) {
		int pin = k < 3 ? k : 7, edg = m->tctl2 >> (2 * k) & 3, rise = (in >> pin) & 1;
		if (!(ch >> pin & 1) || !edg)
			continue;
		if (k == 3 && !(m->pactl & 0x04))
			continue;
		if ((edg == 1 && rise) || (edg == 2 && !rise) || edg == 3) {
			if (k < 3) {
				m->tic[k] = m->tcnt;
				m->tflg1 |= (uint8_t)(1 << k);
			} else {
				m->ti4o5 = m->tcnt;
				m->tflg1 |= 0x80;
			}
		}
	}
	m->gp_in_last = in;
}

static uint16_t
gpt_read(hc16z1 *m, uint32_t a, int bytes)
{
	uint16_t v = 0;
	switch (a & 0x3E) {
	case 0x00: return m->gptmcr;
	case 0x04: return m->gpticr;
	case 0x06: {
		/* PORTGP reads the pins: outputs as driven, inputs from outside */
		uint8_t in = m->board.pins_in(m->board.ctx, HC16_PORTGP);
		return (uint16_t)(m->ddrgp << 8 | ((m->portgp & m->ddrgp) | (in & ~m->ddrgp)));
	}
	case 0x08: return (uint16_t)(m->oc1m << 8 | m->oc1d);
	case 0x0A: return m->tcnt;
	case 0x0C: return (uint16_t)(m->pactl << 8 | m->pacnt);
	case 0x0E: case 0x10: case 0x12: return m->tic[((a & 0x3E) - 0x0E) / 2];
	case 0x14: case 0x16: case 0x18: case 0x1A: return m->toc[((a & 0x3E) - 0x14) / 2];
	case 0x1C: return m->ti4o5;
	case 0x1E: return (uint16_t)(m->tctl1 << 8 | m->tctl2);
	case 0x20: return (uint16_t)(m->tmsk1 << 8 | m->tmsk2);
	case 0x22:
		if (bytes & 2) m->tflg1_read = m->tflg1;
		if (bytes & 1) m->tflg2_read = m->tflg2;
		return (uint16_t)(m->tflg1 << 8 | m->tflg2);
	case 0x24: return (uint16_t)(m->pwmc & ~0x03) | ((m->pwmc & 0x03));
	case 0x26: return (uint16_t)(m->pwma << 8 | m->pwmb);
	case 0x28: return m->pwmcnt;
	case 0x2A: return (uint16_t)(m->pwmbufa << 8 | m->pwmbufb);
	case 0x2C: return m->prescl;
	}
	return v;
}

static void
gpt_write(hc16z1 *m, uint32_t a, uint16_t v, int bytes)
{
	uint8_t hi = (uint8_t)(v >> 8), lo = (uint8_t)v;
	switch (a & 0x3E) {
	case 0x00:
		if (bytes & 2) m->gptmcr = (uint16_t)((m->gptmcr & 0x00FF) | (v & 0xF700));
		if (bytes & 1) m->gptmcr = (uint16_t)((m->gptmcr & 0xFF00) | (lo & 0x8F));
		break;
	case 0x04:
		if (bytes & 2) m->gpticr = (uint16_t)((m->gpticr & 0x00FF) | (v & 0xF700));
		if (bytes & 1) m->gpticr = (uint16_t)((m->gpticr & 0xFF00) | (lo & 0xF0));
		break;
	case 0x06:
		if (bytes & 2) m->ddrgp = hi;
		if (bytes & 1) m->portgp = lo;
		pins(m, HC16_PORTGP);
		break;
	case 0x08:
		if (bytes & 2) m->oc1m = hi & 0xF8;
		if (bytes & 1) m->oc1d = lo & 0xF8;
		break;
	case 0x0C:
		if (bytes & 2) m->pactl = (uint8_t)((m->pactl & 0x88) | (hi & 0x77));
		if (bytes & 1) m->pacnt = lo;
		break;
	case 0x14: case 0x16: case 0x18: case 0x1A: {
		uint16_t *p = &m->toc[((a & 0x3E) - 0x14) / 2];
		if (bytes & 2) *p = (uint16_t)((*p & 0x00FF) | (v & 0xFF00));
		if (bytes & 1) *p = (uint16_t)((*p & 0xFF00) | lo);
		break;
	}
	case 0x1C:
		if (m->pactl & 0x04)
			break;			/* IC4: read-only */
		if (bytes & 2) m->ti4o5 = (uint16_t)((m->ti4o5 & 0x00FF) | (v & 0xFF00));
		if (bytes & 1) m->ti4o5 = (uint16_t)((m->ti4o5 & 0xFF00) | lo);
		break;
	case 0x1E:
		if (bytes & 2) m->tctl1 = hi;
		if (bytes & 1) m->tctl2 = lo;
		break;
	case 0x20:
		if (bytes & 2) m->tmsk1 = hi;
		if (bytes & 1) {
			uint8_t cpr = m->tmsk2_cpr_written ? (m->tmsk2 & 7) : (lo & 7);
			m->tmsk2 = (uint8_t)((lo & 0xB8) | cpr);
			m->tmsk2_cpr_written = 1;
		}
		break;
	case 0x22:
		/* a flag clears when written 0 after a read saw it set */
		if (bytes & 2) {
			m->tflg1 &= (uint8_t)~(m->tflg1_read & ~hi);
			m->tflg1_read = 0;
		}
		if (bytes & 1) {
			m->tflg2 &= (uint8_t)~(m->tflg2_read & ~lo);
			m->tflg2_read = 0;
		}
		break;
	case 0x24:
		if (bytes & 2) {		/* CFORC: forced compares, no flags */
			for (int k = 1; k <= 5; k++)
				if (hi & (0x08 << (k - 1)))
					oc_action(m, k);
		}
		if (bytes & 1)
			m->pwmc = lo;
		break;
	case 0x26:
		if (bytes & 2) m->pwma = hi;
		if (bytes & 1) m->pwmb = lo;
		break;
	}
	update_irq(m);
}

/* ---------------------------------------------------------------- ADC */

/* clocks of one conversion: 2 + 2 + final sample (2/4/8/16) + 10 or 12
 * ADC clocks (ADCRM 5.3).  OH5NXO measured 20 ADC clocks per 10-bit
 * conversion in multichannel scan on the MDR (afsk.s: "the undocumented
 * 2 extra cycles from input mux change ?"), and his demodulator's bit
 * timing (8066 Hz audio rate) is tuned to that: 2 more when MULT = 1 */
static uint64_t
adc_conv(const hc16z1 *m)
{
	unsigned prs = m->adctl0 & 0x1F, sts = m->adctl0 >> 5 & 3;
	unsigned clk = 2 + 2 + (2u << sts) + ((m->adctl0 & 0x80) ? 12 : 10);
	if (m->adctl1 & 0x20)
		clk += 2;
	return (uint64_t)clk * 2 * ((prs ? prs : 1) + 1);
}

static int
adc_count(const hc16z1 *m)
{
	return (m->adctl1 & 0x10) ? 8 : 4;
}

/* input converted by result slot k */
static int
adc_input(const hc16z1 *m, int k)
{
	unsigned c = m->adctl1 & 0xF;
	if (!(m->adctl1 & 0x20))
		return (int)c;
	if (m->adctl1 & 0x10)
		return (c & 8) ? (k < 4 ? 8 + k : 12 + (k - 4)) : k;
	return (int)((c & 0xC) | (unsigned)k);
}

static int
adc_sample(hc16z1 *m, int in)
{
	int v;
	if (in < 8)
		v = m->board.analog(m->board.ctx, in);
	else if (in == 12)
		v = 1023;
	else if (in == 14)
		v = 512;
	else
		v = 0;
	if (v < 0) v = 0;
	if (v > 1023) v = 1023;
	return v;
}

static void
adc_tick(hc16z1 *m)
{
	while (m->adc_running && m->clk >= m->adc_next) {
		int k = m->adc_seq, n = adc_count(m);
		int v = adc_sample(m, adc_input(m, k));
		m->rslt[k] = (uint16_t)((m->adctl0 & 0x80) ? v : v >> 2);
		m->adcstat |= (uint16_t)(1 << k);
		k++;
		if (k >= n) {
			m->adcstat |= 0x8000;	/* SCF */
			k = 0;
			if (!(m->adctl1 & 0x40))
				m->adc_running = 0;
		}
		m->adc_seq = k;
		m->adcstat = (uint16_t)((m->adcstat & ~0x0700) | (k << 8));
		m->adc_next += adc_conv(m);
	}
}

static uint16_t
adc_read(hc16z1 *m, uint32_t a, int bytes)
{
	unsigned off = a & 0x3E;
	if (off >= 0x10) {
		int k = (int)((off & 0xE) >> 1), bits = (m->adctl0 & 0x80) ? 10 : 8;
		uint16_t r = m->rslt[k];
		if (bytes)
			m->adcstat &= (uint16_t)~(1 << k);
		if (off < 0x20)
			return r;
		if (off < 0x30)
			return (uint16_t)((r ^ (1u << (bits - 1))) << (16 - bits));
		return (uint16_t)(r << (16 - bits));
	}
	switch (off) {
	case 0x00: return m->adcmcr;
	case 0x06: return m->board.pins_in(m->board.ctx, HC16_PORTADA);
	case 0x0A: return m->adctl0;
	case 0x0C: return m->adctl1;
	case 0x0E: return m->adcstat;
	}
	return 0;
}

static void
adc_write(hc16z1 *m, uint32_t a, uint16_t v, int bytes)
{
	switch (a & 0x3E) {
	case 0x00:
		if (bytes & 2) m->adcmcr = (uint16_t)((m->adcmcr & 0x00FF) | (v & 0xE000));
		if (bytes & 1) m->adcmcr = (uint16_t)((m->adcmcr & 0xFF00) | (v & 0x80));
		if (m->adcmcr & 0x8000)
			m->adc_running = 0;
		return;
	case 0x0A:
		if (bytes & 2) m->adctl0 = (uint16_t)((m->adctl0 & 0x00FF) | (v & 0xFF00));
		if (bytes & 1) m->adctl0 = (uint16_t)((m->adctl0 & 0xFF00) | (v & 0xFF));
		return;
	case 0x0C:
		/* starts a new sequence: SCF, CCTR and the CCFs reset */
		if (bytes & 1) m->adctl1 = v & 0x7F;
		m->adcstat = 0;
		m->adc_seq = 0;
		m->adc_running = !(m->adcmcr & 0x8000);
		m->adc_next = m->clk + adc_conv(m);
		return;
	}
}

/* ---------------------------------------------------------------- registers */

static uint16_t
reg_word(hc16z1 *m, uint32_t a, int bytes)
{
	uint32_t off = a & 0xFFF;
	if (off >= 0x700 && off < 0x740)
		return adc_read(m, a, bytes);
	if (off >= 0x900 && off < 0x940)
		return gpt_read(m, a, bytes);
	if (off >= 0xA00 && off < 0xA80)
		return sim_read(m, a);
	if (off >= 0xB00 && off < 0xB08)
		return sram_read(m, a);
	if (off >= 0xC00 && off < 0xE00)
		return qsm_read(m, a, bytes);
	return 0;
}

static uint16_t
reg_read(hc16z1 *m, uint32_t a, int size)
{
	uint16_t w;
	if (size == 2)
		return reg_word(m, a & ~1u, 3);
	w = reg_word(m, a & ~1u, (a & 1) ? 1 : 2);
	return (a & 1) ? (w & 0xFF) : (w >> 8);
}

static void
reg_write(hc16z1 *m, uint32_t a, int size, uint16_t v)
{
	uint32_t off = a & 0xFFF, w = a & ~1u;
	int bytes = size == 2 ? 3 : (a & 1) ? 1 : 2;
	if (size == 1 && !(a & 1))
		v = (uint16_t)(v << 8);
	if (off >= 0x700 && off < 0x740)
		adc_write(m, w, v, bytes);
	else if (off >= 0x900 && off < 0x940)
		gpt_write(m, w, v, bytes);
	else if (off >= 0xA00 && off < 0xA80)
		sim_write(m, w, v, bytes);
	else if (off >= 0xB00 && off < 0xB08)
		sram_write(m, w, v, bytes);
	else if (off >= 0xC00 && off < 0xE00)
		qsm_write(m, w, v, bytes);
	update_irq(m);
}

/* ---------------------------------------------------------------- api */

void
hc16z1_init(hc16z1 *m, const hc16z1_board *b)
{
	cpu16_bus bus = { m, bus_rd8, bus_wr8, bus_rd16, bus_wr16, bus_clocks, bus_iack };
	memset(m, 0, sizeof(*m));
	m->board = *b;
	cpu16_init(&m->cpu, &bus);
}

void
hc16z1_reset(hc16z1 *m)
{
	uint64_t clk = m->clk;
	hc16z1_board b = m->board;
	cpu16 cpu = m->cpu;
	uint8_t ram[1024];
	memcpy(ram, m->ram, sizeof(ram));	/* the standby RAM keeps its contents */
	memset(m, 0, sizeof(*m));
	m->board = b;
	m->cpu = cpu;
	m->clk = clk;
	memcpy(m->ram, ram, sizeof(ram));

	m->simcr = 0x00CF;
	m->syncr = 0x3F00;
	m->sypcr = 0x80;		/* watchdog on, 2^9 EXTAL cycles */
	m->picr = 0x000F;
	m->portc = 0x7F;
	m->pepar = m->pfpar = 0xFF;
	m->cspar[0] = 0x03FE;		/* DATA0 low: 8-bit boot port */
	m->cspar[1] = 0x03FF;
	m->csbar[11] = 0x0007;
	m->csor[11] = 0x7B70;
	m->rammcr = 0x8000;
	m->gptmcr = 0x0080;
	m->tic[0] = m->tic[1] = m->tic[2] = 0xFFFF;
	m->toc[0] = m->toc[1] = m->toc[2] = m->toc[3] = 0xFFFF;
	m->ti4o5 = 0xFFFF;
	m->qilr = 0;
	m->qivr = 0x0F;
	m->scsr = SC_TDRE | SC_TC;
	m->adcmcr = 0x8080;
	m->adctl0 = 0x0003;
	m->pwm_duty[0] = m->pwm_duty[1] = -1;

	pins(m, HC16_PORTC);
	pins(m, HC16_PORTF);
	pins(m, HC16_PORTGP);
	pins(m, HC16_PORTQS);
	cpu16_reset(&m->cpu);
	m->clk += 40;
}

/* EXTAL cycles: the PIT and the watchdog */
static void
extal_tick(hc16z1 *m, int n)
{
	uint32_t f = fsys(m);
	m->ext_frac += (uint64_t)n * HC16_EXTAL;
	while (m->ext_frac >= f) {
		uint32_t pitm = m->pitr & 0xFF;
		m->ext_frac -= f;
		if (pitm && !m->cpu.stopped) {
			uint32_t period = pitm * 4 * ((m->pitr & 0x100) ? 512 : 1);
			if (++m->pit_count >= period) {
				m->pit_count = 0;
				m->pit_pending = 1;
				update_irq(m);
			}
		}
		if ((m->sypcr & 0x80) && !m->cpu.stopped && ++m->wd_count >= wd_ratio(m)) {
			m->board.reset(m->board.ctx, HC16_RESET_WATCHDOG);
			hc16z1_reset(m);
			m->rsr = 0x20;		/* SW: software watchdog reset */
			return;
		}
	}
}

int
hc16z1_step(hc16z1 *m)
{
	int n = cpu16_step(&m->cpu);
	m->clk += (uint64_t)n;
	gpt_tick(m, n);
	gpt_inputs(m);
	sci_tick(m);
	adc_tick(m);
	extal_tick(m, n);
	update_irq(m);
	return n;
}

uint8_t
hc16z1_peek(hc16z1 *m, uint32_t a)
{
	if (REG(a))
		return (uint8_t)((a & 1) ? reg_word(m, a & ~1u, 0) : reg_word(m, a & ~1u, 0) >> 8);
	if (in_ram(m, a))
		return m->ram[a & 0x3FF];
	return (uint8_t)ext_read(m, a, 1);
}

void
hc16z1_poke(hc16z1 *m, uint32_t a, uint8_t v)
{
	bus_wr8(m, a, v);
}
