/*
 * TMF-1 / TMN-1 radio unit and its MBUS; see tmx1.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tmx1.h"

#define BIT_CLK 1152		/* clocks per MBUS bit: 9600 bd */

/* port bits (tmx1.asm) */
#define PA_SCL    0x01
#define PA_SDA    0x02
#define PA_LFUSTB 0x10
#define PB_TSEN   0x02
#define PB_RSEN   0x04
#define PB_TXON   0x10
#define PC_I2DA   0x04
#define PC_PAGE   0x08
#define PC_DADIS  0x10
#define PC_WDCLR  0x40
#define PC_PWROFF 0x80

/* MAS7825 status (OH5NXO MAS.registers, as the firmware polls it) */
#define ST_RFLAG 0x01
#define ST_TFLAG 0x04
#define ST_TXE   0x08

static void
ev(tmx1 *m, int type, int arg)
{
	tmx1_event *e;
	if (m->ev_n == TMX1_EVQ) {	/* drop the oldest */
		m->ev_head = (m->ev_head + 1) % TMX1_EVQ;
		m->ev_n--;
	}
	e = &m->ev[(m->ev_head + m->ev_n++) % TMX1_EVQ];
	e->at = m->clk;
	e->type = type;
	e->arg = arg;
}

int
tmx1_event_pop(tmx1 *m, tmx1_event *e)
{
	if (!m->ev_n)
		return 0;
	*e = m->ev[m->ev_head];
	m->ev_head = (m->ev_head + 1) % TMX1_EVQ;
	m->ev_n--;
	return 1;
}

double
tmx1_time(const tmx1 *m)
{
	return (double)m->clk / TMX1_HZ;
}

double
tmx1_pll_hz(const tmx1 *m, const tmx1_pll *p)
{
	return p->r ? (double)p->d * m->ref_hz / p->r : 0;
}

/* ---------------------------------------------------------------- MBUS */

static void
snf_edge(tmx1 *m, int level, uint64_t t)
{
	if (!m->snf_t0) {
		if (!level) {
			m->snf_t0 = t ? t : 1;
			m->snf_ne = 0;
			m->snf_from = (!m->radio_txd ? TMX1_MB_RADIO : 0)
			    | (!m->hs_txd ? TMX1_MB_HS : 0)
			    | (!m->host_txd ? TMX1_MB_HOST : 0);
		}
		return;
	}
	if (m->snf_ne < (int)(sizeof(m->snf_edge) / sizeof(m->snf_edge[0])))
		m->snf_edge[m->snf_ne++] = t;
}

/* decode a frame once its stop bit has been sampled: 8 data, odd parity */
static void
snf_check(tmx1 *m, uint64_t now)
{
	uint64_t t0 = m->snf_t0;
	unsigned frame = 0;
	int k, e = 0, lvl = 0, err = 0, ones;

	if (!t0 || now < t0 + BIT_CLK * 21 / 2)
		return;
	for (k = 1; k <= 10; k++) {
		uint64_t ts = t0 + (uint64_t)BIT_CLK * k + BIT_CLK / 2;
		while (e < m->snf_ne && m->snf_edge[e] <= ts) {
			lvl ^= 1;
			e++;
		}
		frame |= (unsigned)lvl << (k - 1);
	}
	ones = __builtin_popcount(frame & 0x1FF);
	if (!(ones & 1) || !(frame & 0x200))
		err = 1;
	ev(m, TMX1_EV_MBUS, (int)((frame & 0xFF) | (unsigned)err << 8 | (unsigned)m->snf_from << 9));
	m->snf_t0 = 0;		/* the next frame starts at the next falling edge */
}

static void
line_update(tmx1 *m, uint64_t t)
{
	int l = m->radio_txd & m->hs_txd & m->host_txd;
	if (l == m->line)
		return;
	m->line = l;
	m->line_t = t;
	snf_edge(m, l, t);
	upd7810_set_rxd(&m->cpu, l);
	upd7810_set_ci(&m->cpu, l);
	if (m->hs.type != TMX1_HS_NONE) {
		upd7810_set_rxd(&m->hs.cpu, l);
		upd7810_set_ci(&m->hs.cpu, l);
	}
}

static void
radio_txd(void *ctx, int level)
{
	tmx1 *m = ctx;
	m->radio_txd = level || !m->powered;
	line_update(m, m->clk);
}

static void
hs_txd(void *ctx, int level)
{
	tmx1 *m = ctx;
	m->hs_txd = level || !m->powered;
	line_update(m, m->hs_clk);
}

static void
hs_event(void *ctx, int type, int arg)
{
	tmx1 *m = ctx;
	if (type == TMX1HS_EV_DTMF)
		ev(m, TMX1_EV_DTMF, arg);
}

void
tmx1_mbus_send(tmx1 *m, uint8_t b)
{
	if (m->hq_n < TMX1_HOSTQ)
		m->hostq[(m->hq_head + m->hq_n++) % TMX1_HOSTQ] = b;
}

static void
host_tx(tmx1 *m)
{
	int lvl = 1;
	/* a packet starts on a free bus (line high 2 ms, as the units'
	 * ECNT > 1000 check); its bytes follow back to back */
	if (!m->host_t0 && m->hq_n
	    && (m->host_burst || (m->line && m->clk - m->line_t >= (uint64_t)(TMX1_HZ * 0.002)))) {
		uint8_t b = m->hostq[m->hq_head];
		int ones = __builtin_popcount(b);
		m->hq_head = (m->hq_head + 1) % TMX1_HOSTQ;
		m->hq_n--;
		/* start, 8 data, odd parity, stop */
		m->host_frame = (uint16_t)(b << 1 | (!(ones & 1)) << 9 | 1 << 10);
		m->host_t0 = m->clk ? m->clk : 1;
		m->host_burst = 1;
	}
	if (m->host_t0) {
		uint64_t i = (m->clk - m->host_t0) / BIT_CLK;
		if (i >= 11) {
			m->host_t0 = 0;
			m->host_burst = m->hq_n != 0;
		}
		else
			lvl = (m->host_frame >> i) & 1;
	}
	if (lvl != m->host_txd) {
		m->host_txd = lvl;
		line_update(m, m->clk);
	}
}

/* ----------------------------------------------------------------- PIT */

static void
pit_out(void *ctx, int counter, int level, unsigned offset)
{
	tmx1 *m = ctx;
	if (counter != 2)
		return;
	m->out2 = level;
	if (m->aud_on) {
		if (m->aud_n == m->aud_cap) {
			unsigned cap = m->aud_cap ? m->aud_cap * 2 : 65536;
			uint64_t *t = realloc(m->aud_t, cap * sizeof(*t));
			uint8_t *v = realloc(m->aud_v, cap);
			if (!t || !v) {
				free(t);
				free(v);
				m->aud_t = NULL;
				m->aud_v = NULL;
				m->aud_on = 0;
				return;
			}
			m->aud_t = t;
			m->aud_v = v;
			m->aud_cap = cap;
		}
		m->aud_t[m->aud_n] = (m->pit_done12 + offset) * 12;
		m->aud_v[m->aud_n++] = (uint8_t)level;
	}
}

static void
pit_sync(tmx1 *m)
{
	uint64_t n12 = m->clk / 12, n0;
	if (n12 > m->pit_done12) {
		unsigned n = (unsigned)(n12 - m->pit_done12);
		m->pit.quiet = 3 | (m->aud_on ? 0 : 4);
		pit_clock(&m->pit, 1, n);
		pit_clock(&m->pit, 2, n);
		m->pit_done12 = n12;
	}
	n0 = (uint64_t)((double)m->clk * m->if_hz / TMX1_HZ);
	if (n0 > m->pit_done0) {
		pit_clock(&m->pit, 0, (unsigned)(n0 - m->pit_done0));
		m->pit_done0 = n0;
	}
}

void
tmx1_audio_capture(tmx1 *m, int on)
{
	pit_sync(m);
	m->aud_on = on;
	if (on)
		m->aud_n = 0;
}

/* --------------------------------------------------------------- modem */

static void
modem_reset(tmx1 *m)
{
	m->m_ctrl = 0;
	m->m_stat = ST_TFLAG;
	m->m_tx_done = 0;
}

void
tmx1_modem_rx(tmx1 *m, uint8_t b)
{
	m->m_rx = b;
	m->m_stat |= ST_RFLAG;
}

static void
modem_tick(tmx1 *m)
{
	if (m->m_tx_done && m->clk >= m->m_tx_done) {
		m->m_tx_done = 0;
		m->m_stat |= ST_TFLAG;
		m->m_stat &= (uint8_t)~ST_TXE;
	}
}

/* ------------------------------------------------------------- memory */

static uint8_t
bus_read(void *ctx, uint16_t a)
{
	tmx1 *m = ctx;
	if (a < 0x8000)
		return m->rom[a];
	if (a < 0xC000)		/* PAGE 1: C000-FFFF of the EPROM here */
		return m->rom[a + ((m->pc & PC_PAGE) ? 0x4000 : 0)];
	if (a >= 0xE000)
		return m->ram[a - 0xE000];
	if (a < 0xC400) {	/* MENA: MAS7825 */
		modem_tick(m);
		if (a & 1)
			return m->m_stat;
		m->m_stat &= (uint8_t)~ST_RFLAG;
		return m->m_rx;
	}
	if (a < 0xC800) {	/* TENA: i8253 */
		pit_sync(m);
		return pit_read(&m->pit, a & 3);
	}
	return 0xFF;
}

static void
bus_write(void *ctx, uint16_t a, uint8_t v)
{
	tmx1 *m = ctx;
	if (a >= 0xE000) {
		m->ram[a - 0xE000] = v;
		if (a >= m->watch_lo && a <= m->watch_hi && m->watch_hi) {
			m->watch_hit = 1;
			m->watch_addr = a;
		}
		return;
	}
	if (a >= 0xC000 && a < 0xC400) {
		modem_tick(m);
		if (a & 1)
			m->m_ctrl = v;
		else {
			ev(m, TMX1_EV_MODEM_TX, v);
			m->m_stat &= (uint8_t)~ST_TFLAG;
			m->m_stat |= ST_TXE;
			m->m_tx_done = m->clk + (uint64_t)(TMX1_HZ * 8 / 1200);
		}
		return;
	}
	if (a >= 0xC400 && a < 0xC800) {
		pit_sync(m);
		pit_write(&m->pit, a & 3, v);
		return;
	}
	if (a >= 0xD400 && a < 0xD800) {	/* AMU/LE 74259: A2..A0, D0 */
		uint8_t b = (uint8_t)(1 << (a & 7));
		m->dev = (uint8_t)((m->dev & ~b) | ((v & 1) ? b : 0));
		ev(m, TMX1_EV_DEV, m->dev & 7);
	}
}

/* --------------------------------------------------------------- ports */

static uint8_t
port_in(void *ctx, int p)
{
	tmx1 *m = ctx;
	switch (p) {
	case UPD_PA:	/* DATA, DTC: unused inputs with 100k pull-downs */
		return 0x77;
	case UPD_PC:	/* I2DA = /PTT, 10k pull-up */
		return (uint8_t)(0xFB | (m->ptt ? 0 : PC_I2DA));
	}
	return 0xFF;
}

static void
pll_latch(tmx1 *m, tmx1_pll *p, int which)
{
	uint64_t s = m->sbits;
	if (s & 1) {		/* control bit 1: reference divider */
		p->r = (uint32_t)(s >> 1) & 0x3FFF;
		p->sw = (uint32_t)(s >> 15) & 1;
	} else {		/* 0: N (11 bits) and A (7 bits), linear */
		p->d = (uint32_t)(s >> 1) & 0x3FFFF;
		p->loads++;
		ev(m, TMX1_EV_SYNTH, which);
	}
}

static void power_off(tmx1 *m);

static void
port_out(void *ctx, int p, uint8_t latch, uint8_t mode)
{
	tmx1 *m = ctx;
	uint8_t pins = (uint8_t)(latch | mode), old;

	switch (p) {
	case UPD_PA:
		old = m->pa;
		m->pa = pins;
		if ((pins & PA_SCL) && !(old & PA_SCL))
			m->sbits = m->sbits << 1 | ((pins & PA_SDA) ? 1 : 0);
		if (!(pins & PA_SCL) && (old & PA_SCL) && !(m->pc & PC_DADIS)) {
			m->dbits = m->dbits << 1 | ((pins & PA_SDA) ? 1 : 0);
			m->dnbits++;
		}
		if ((pins & PA_LFUSTB) && !(old & PA_LFUSTB)) {
			uint8_t b = (uint8_t)m->sbits;
			m->lfu[b >> 6] = b & 0x3F;
			m->lfu_writes++;
			ev(m, TMX1_EV_LFU, b);
		}
		break;
	case UPD_PB:
		old = m->pb;
		m->pb = pins;
		if ((pins & PB_RSEN) && !(old & PB_RSEN))
			pll_latch(m, &m->rx, 0);
		if ((pins & PB_TSEN) && !(old & PB_TSEN))
			pll_latch(m, &m->tx, 1);
		if ((pins ^ old) & PB_TXON)
			ev(m, (pins & PB_TXON) ? TMX1_EV_TX_ON : TMX1_EV_TX_OFF, 0);
		break;
	case UPD_PC:
		old = m->pc;
		m->pc = pins;
		if ((pins & PC_WDCLR) && !(old & PC_WDCLR)) {
			m->wd_last = m->clk;
			m->wd_nmi_sent = 0;
		}
		if (!(pins & PC_DADIS) && (old & PC_DADIS)) {
			m->dbits = 0;
			m->dnbits = 0;
		}
		if ((pins & PC_DADIS) && !(old & PC_DADIS) && m->dnbits >= 24) {
			for (int i = 0; i < 4; i++)
				m->dac[i] = (uint8_t)(m->dbits >> (18 - 6 * i)) & 0x3F;
			m->dac_loads++;
			ev(m, TMX1_EV_DAC, 0);
		}
		if ((pins & PC_PWROFF) && !(old & PC_PWROFF) && m->powered)
			power_off(m);
		break;
	}
}

static uint8_t
adc(void *ctx, int ch)
{
	tmx1 *m = ctx;
	return m->an[ch & 7];
}

/* --------------------------------------------------------------- power */

static void
power_off(tmx1 *m)
{
	m->powered = 0;
	ev(m, TMX1_EV_POWEROFF, 0);
	m->radio_txd = m->hs_txd = 1;
	line_update(m, m->clk);
}

void
tmx1_power(tmx1 *m, int on)
{
	if (!on) {
		if (m->powered)
			power_off(m);
		return;
	}
	if (m->powered)
		return;
	m->powered = 1;
	m->radio_txd = m->hs_txd = 1;
	m->pa = m->pb = m->pc = 0xFF;
	upd7810_reset(&m->cpu);
	modem_reset(m);
	m->wd_last = m->clk;
	m->wd_nmi_sent = 0;
	if (m->hs.type != TMX1_HS_NONE) {
		tmx1hs_reset(&m->hs);
		m->hs_clk = m->clk;
	}
	ev(m, TMX1_EV_POWERON, 0);
}

void
tmx1_power_key(tmx1 *m, int down)
{
	if (m->hs.type != TMX1_HS_NONE)
		tmx1hs_power_key(&m->hs, down);
	/* the power logic switches the unit on from the handset's key */
	if (down && !m->powered)
		tmx1_power(m, 1);
}

/* ---------------------------------------------------------------- run */

uint8_t
tmx1_peek(tmx1 *m, uint16_t a)
{
	if (a >= 0xE000)
		return m->ram[a - 0xE000];
	if (a < 0xC000)
		return bus_read(m, a);
	return 0xFF;
}

void
tmx1_poke(tmx1 *m, uint16_t a, uint8_t v)
{
	if (a >= 0xE000)
		m->ram[a - 0xE000] = v;
	else if (a < 0xC000)
		m->rom[a] = v;
}

void
tmx1_breakpoint(tmx1 *m, uint16_t a, int on)
{
	uint8_t b = (uint8_t)(1 << (a & 7));
	if (on && !(m->bp[a >> 3] & b)) {
		m->bp[a >> 3] |= b;
		m->nbp++;
	} else if (!on && (m->bp[a >> 3] & b)) {
		m->bp[a >> 3] &= (uint8_t)~b;
		m->nbp--;
	}
}

static void
periodic(tmx1 *m)
{
	uint64_t since = m->clk - m->wd_last;
	host_tx(m);
	snf_check(m, m->clk);
	if (!m->wd_nmi_sent && since > (uint64_t)(m->wd_nmi_s * TMX1_HZ)) {
		m->wd_nmi_sent = 1;
		upd7810_nmi(&m->cpu);
		ev(m, TMX1_EV_WDNMI, m->cpu.pc);
	}
	if (since > (uint64_t)(m->wd_off_s * TMX1_HZ))
		power_off(m);
	if (m->clk - m->pit_done12 * 12 >= 1024)
		pit_sync(m);
}

/* one radio instruction, running the handset up to the radio's time
 * first; returns a TMX1_STOP_* code */
int
tmx1_step(tmx1 *m)
{
	int n;

	if (!m->powered) {
		m->clk += 12;
		m->hs_clk = m->clk;
		host_tx(m);
		snf_check(m, m->clk);
		return TMX1_STOP_OFF;
	}
	if (m->hs.type != TMX1_HS_NONE) {
		while (m->hs_clk <= m->clk) {
			n = upd7810_step(&m->hs.cpu);
			m->hs_clk += (uint64_t)n * 3;
			if (m->hs.cpu.illegal) {
				m->hs.cpu.illegal = 0;
				ev(m, TMX1_EV_ILLEGAL, m->hs.cpu.op_pc | 0x10000);
			}
		}
	}
	if (m->nbp && !m->skip_bp && !m->cpu.halt
	    && (m->bp[m->cpu.pc >> 3] & (1 << (m->cpu.pc & 7)))) {
		m->skip_bp = 1;
		return TMX1_STOP_BREAK;
	}
	m->skip_bp = 0;
	if (!m->cpu.halt) {
		m->trace[m->trace_pos] = m->cpu.pc;
		m->trace_pos = (m->trace_pos + 1) % TMX1_TRACE;
	}
	n = upd7810_step(&m->cpu);
	m->clk += (uint64_t)n * 3;
	if (m->cpu.illegal) {
		m->cpu.illegal = 0;
		ev(m, TMX1_EV_ILLEGAL, m->cpu.op_pc);
	}
	periodic(m);
	if (m->watch_hit) {
		m->watch_hit = 0;
		return TMX1_STOP_WATCH;
	}
	return TMX1_STOP_TIME;
}

int
tmx1_run(tmx1 *m, double seconds)
{
	uint64_t end = m->clk + (uint64_t)(seconds * TMX1_HZ);
	int r;
	while (m->clk < end) {
		if (!m->powered) {
			/* time passes; the host transmitter and the monitor still run */
			while (m->clk < end && !m->powered)
				tmx1_step(m);
			continue;
		}
		r = tmx1_step(m);
		if (r == TMX1_STOP_BREAK || r == TMX1_STOP_WATCH)
			return r;
	}
	pit_sync(m);
	return m->powered ? TMX1_STOP_TIME : TMX1_STOP_OFF;
}

/* ---------------------------------------------------------------- init */

static int
load(const char *path, uint8_t *dst, size_t max)
{
	FILE *f = fopen(path, "rb");
	size_t n;
	if (!f)
		return -1;
	n = fread(dst, 1, max, f);
	fclose(f);
	return (int)n;
}

int
tmx1_load_rom(tmx1 *m, const char *path)
{
	memset(m->rom, 0xFF, sizeof(m->rom));
	return load(path, m->rom, sizeof(m->rom));
}

int
tmx1_load_hs_rom(tmx1 *m, const char *path)
{
	memset(m->hs.rom, 0xFF, sizeof(m->hs.rom));
	return load(path, m->hs.rom, sizeof(m->hs.rom));
}

void
tmx1_init(tmx1 *m, int handset)
{
	upd7810_bus b = { 0 };

	memset(m, 0, sizeof(*m));
	memset(m->rom, 0xFF, sizeof(m->rom));
	m->ref_hz = 12.8e6;
	m->if_hz = 455000.0;
	m->wd_nmi_s = 0.4;
	m->wd_off_s = 12.0;
	m->radio_txd = m->hs_txd = m->host_txd = m->line = 1;
	m->pa = m->pb = m->pc = 0xFF;
	/* idle A/D readings: 13.8 V battery, +20 C, no signal */
	m->an[TMX1_AN_FSKL] = 0;
	m->an[TMX1_AN_BCR] = 0;
	m->an[TMX1_AN_BTMP] = 128;
	m->an[TMX1_AN_RSSI] = 0;
	m->an[TMX1_AN_PFB] = 0;
	m->an[TMX1_AN_BATT] = 200;
	m->an[TMX1_AN_TEMP] = 153;
	m->an[TMX1_AN_TIMEOUT] = 255;

	pit_init(&m->pit);
	m->pit.out_changed = pit_out;
	m->pit.ctx = m;
	modem_reset(m);

	b.ctx = m;
	b.read = bus_read;
	b.write = bus_write;
	b.port_in = port_in;
	b.port_out = port_out;
	b.adc = adc;
	b.txd = radio_txd;
	upd7810_init(&m->cpu, &b);

	m->hs.board = m;
	m->hs.on_txd = hs_txd;
	m->hs.on_event = hs_event;
	tmx1hs_init(&m->hs, handset);
	m->radio_txd = m->hs_txd = m->host_txd = m->line = 1;
}
