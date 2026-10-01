/*
 * MC25 TVL/PTL logic.  See mc25.h and notes/mc25.md.
 */
#include "mc25.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define O1_SDO      0x01
#define O1_KEYCLK   0x02
#define O1_DPYCLK   0x04
#define O1_DPY_ON   0x08
#define O1_SYNTH_P0 0x20
#define O3_TXON     0x10
#define O5_ACK_EV   0x02
#define O5_ACK_ARP  0x04

#define I4_HOOK     0x01
#define I4_SQUELCH  0x02
#define I4_LOCK     0x04
#define I4_PROGRAM  0x08
#define I4_OFFICE   0x10
#define I4_1800     0x20
#define I4_EV       0x40
#define I4_ARP      0x80

#define TICK        (MC25_HZ / 1000.0)		/* 1 ms interrupt */
#define CCIR_TONE_S 0.100			/* CCIR tone length */
#define SER_BIT     (MC25_HZ / 1200.0)

static void
event(mc25 *m, int type, int arg)
{
	unsigned i = (m->ev_head + m->ev_n) % MC25_EVQ;
	if (m->ev_n == MC25_EVQ) {
		m->ev_head = (m->ev_head + 1) % MC25_EVQ;
		m->ev_n--;
	}
	m->ev[i].at = m->clk;
	m->ev[i].type = type;
	m->ev[i].arg = arg;
	m->ev_n++;
}

int
mc25_event_pop(mc25 *m, mc25_event *e)
{
	if (!m->ev_n)
		return 0;
	*e = m->ev[m->ev_head];
	m->ev_head = (m->ev_head + 1) % MC25_EVQ;
	m->ev_n--;
	return 1;
}

/* ---------------------------------------------------------------- serial */

/* line level on /EF2 as a flag: 1 = mark (idle, pin low); 1200 8E1 */
static int
ser_mark(mc25 *m)
{
	if (!m->ser_busy) {
		if (!m->ser_n)
			return 1;
		m->ser_busy = 1;
		m->ser_t0 = m->clk;
	}
	unsigned bit = (unsigned)((double)(m->clk - m->ser_t0) / SER_BIT);
	uint8_t c = m->ser_q[m->ser_head];
	if (bit == 0)
		return 0;			/* start bit: space */
	if (bit <= 8)
		return (c >> (bit - 1)) & 1;
	if (bit == 9)
		return __builtin_parity(c);	/* even parity */
	if (bit >= 11) {			/* after one stop bit */
		m->ser_head = (m->ser_head + 1) % MC25_SERQ;
		m->ser_n--;
		m->ser_busy = 0;
	}
	return 1;
}

void
mc25_serial_rx(mc25 *m, const uint8_t *bytes, int n)
{
	for (int i = 0; i < n && m->ser_n < MC25_SERQ; i++) {
		m->ser_q[(m->ser_head + m->ser_n) % MC25_SERQ] = bytes[i];
		m->ser_n++;
	}
}

static void
qout(void *ctx, int level)
{
	mc25 *m = ctx;
	if (m->q_n == m->q_cap) {
		m->q_cap = m->q_cap ? m->q_cap * 2 : 4096;
		m->q_t = realloc(m->q_t, m->q_cap * sizeof(*m->q_t));
		m->q_v = realloc(m->q_v, m->q_cap);
	}
	m->q_t[m->q_n] = m->clk;
	m->q_v[m->q_n++] = (uint8_t)level;
}

/* ---------------------------------------------------------------- CCIR */

static void
ccir_present(mc25 *m, int d)
{
	if (m->ccir_cur[d] >= 0 || !m->ccir_n[d] || m->clk < m->ccir_next[d])
		return;
	m->ccir_cur[d] = m->ccir_q[d][m->ccir_head[d]];
	m->ccir_head[d] = (m->ccir_head[d] + 1) % MC25_CCIRQ;
	m->ccir_n[d]--;
	m->ccir_next[d] = m->clk + (uint64_t)(CCIR_TONE_S * MC25_HZ);
}

void
mc25_ccir_rx(mc25 *m, int d, const uint8_t *digits, int n)
{
	d &= 1;
	for (int i = 0; i < n && m->ccir_n[d] < MC25_CCIRQ; i++) {
		m->ccir_q[d][(m->ccir_head[d] + m->ccir_n[d]) % MC25_CCIRQ] = digits[i] & 15;
		m->ccir_n[d]++;
	}
	ccir_present(m, d);
}

/* ---------------------------------------------------------------- I/O */

static uint8_t
in4(mc25 *m)
{
	uint8_t v = 0;
	if (!m->in[MC25_IN_OFFHOOK]) v |= I4_HOOK;
	if (m->in[MC25_IN_SQUELCH])  v |= I4_SQUELCH;
	if (!m->in[MC25_IN_LOCK])    v |= I4_LOCK;
	if (!m->in[MC25_IN_PROGRAM]) v |= I4_PROGRAM;
	if (!m->in[MC25_IN_OFFICE])  v |= I4_OFFICE;
	if (!m->in[MC25_IN_1800])    v |= I4_1800;
	if (m->ccir_cur[0] >= 0)     v |= I4_EV;
	if (m->ccir_cur[1] >= 0)     v |= I4_ARP;
	return v;
}

static uint8_t
ef(void *ctx)
{
	mc25 *m = ctx;
	uint8_t f = 0;
	if (!m->in[MC25_IN_PTT])	/* bn1 = PTT pressed */
		f |= 1;
	if (ser_mark(m))
		f |= 2;
	if (cu41_sdi(&m->cu))		/* SDI inverted three times */
		f |= 4;
	if (m->in[MC25_IN_LOCAL])
		f |= 8;
	return f;
}

static uint8_t
io_in(void *ctx, int n, uint16_t a)
{
	mc25 *m = ctx;
	(void)a;
	switch (n) {
	case 4:
		m->wd_last = m->clk;
		return in4(m);
	case 6: {
		uint8_t ev = m->ccir_cur[0] >= 0 ? (uint8_t)m->ccir_cur[0] : 0;
		uint8_t arp = m->ccir_cur[1] >= 0 ? (uint8_t)m->ccir_cur[1] : 0;
		return (uint8_t)(ev | arp << 4);
	}
	}
	return 0xff;
}

static unsigned
divisor(const mc25 *m)
{
	return (unsigned)m->out[2] << 1 | !!(m->out[1] & O1_SYNTH_P0);
}

static void
io_out(void *ctx, int n, uint16_t a, uint8_t v)
{
	mc25 *m = ctx;
	(void)a;
	uint8_t old = m->out[n & 7];
	unsigned div0 = divisor(m);
	m->out[n & 7] = v;
	switch (n) {
	case 1:
		if ((old ^ v) & (O1_SDO | O1_KEYCLK | O1_DPYCLK | O1_DPY_ON)) {
			unsigned b = m->cu.bytes;
			cu41_out1(&m->cu, v);
			if (m->cu.bytes != b)
				event(m, MC25_EV_DPY, m->cu.last_byte);
		}
		break;
	case 3:
		if ((old ^ v) & O3_TXON)
			event(m, (v & O3_TXON) ? MC25_EV_TX_ON : MC25_EV_TX_OFF, 0);
		break;
	case 5:
		if ((old ^ v) & 0xf0)
			event(m, MC25_EV_CCIR_TX, v >> 4);
		for (int d = 0; d < 2; d++) {
			uint8_t bit = d ? O5_ACK_ARP : O5_ACK_EV;
			if ((v & bit) && !(old & bit) && m->ccir_cur[d] >= 0) {
				m->ccir_cur[d] = -1;
				event(m, MC25_EV_CCIR_ACK, d);
			}
		}
		break;
	}
	if (divisor(m) != div0)
		event(m, MC25_EV_SYNTH, (int)divisor(m));
}

/* ---------------------------------------------------------------- bus */

static uint8_t *
ram_at(mc25 *m, uint16_t a)
{
	return a >= 0x8000 && a < 0xc000 ? &m->ram[a & 0x3ff] : NULL;
}

static uint8_t
mem_read(void *ctx, uint16_t a)
{
	mc25 *m = ctx;
	if (a < 0x4000)
		return m->rom[a];
	if (a < 0x6000)
		return m->eerom_loaded ? m->eerom[a & 0x1fff] : 0xff;
	if (a < 0x8000)
		return 0xff;
	if (a < 0xc000)
		return m->ram[a & 0x3ff];
	return (uint8_t)(0xf0 | m->nv[a & 0xff]);
}

static void
mem_write(void *ctx, uint16_t a, uint8_t v)
{
	mc25 *m = ctx;
	uint8_t *r = ram_at(m, a);
	if (r) {
		*r = v;
		if (a >= m->watch_lo && a <= m->watch_hi) {
			m->watch_hit = 1;
			m->watch_addr = a;
		}
	} else if (a >= 0xc000) {
		m->nv[a & 0xff] = v & 15;
	}
}

static void
inta(void *ctx)
{
	mc25 *m = ctx;
	m->cpu.int_line = 0;
}

/* ---------------------------------------------------------------- setup */

static void
board_reset(mc25 *m)
{
	cdp1802_reset(&m->cpu, CDP1802);
	m->cpu.int_line = 0;
	memset(m->out, 0, sizeof(m->out));
	cu41_out1(&m->cu, 0);		/* /RST low */
	m->wd_last = m->clk;
	m->next_tick = m->clk + (uint64_t)TICK;
}

void
mc25_init(mc25 *m)
{
	memset(m, 0, sizeof(*m));
	memset(m->rom, 0xff, sizeof(m->rom));
	memset(m->nv, 0xf, sizeof(m->nv));
	m->cpu.ctx = m;
	m->cpu.read = mem_read;
	m->cpu.write = mem_write;
	m->cpu.in = io_in;
	m->cpu.out = io_out;
	m->cpu.ef = ef;
	m->cpu.qout = qout;
	m->cpu.inta = inta;
	cu41_init(&m->cu);
	m->ccir_cur[0] = m->ccir_cur[1] = -1;
	m->wd_timeout_s = 0.1;		/* not documented; assumption */
	m->watch_lo = 1;
	board_reset(m);
}

static int
load(const char *path, uint8_t *buf, size_t size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	memset(buf, 0xff, size);
	size_t n = fread(buf, 1, size, f);
	fclose(f);
	return n ? (int)n : -1;
}

int
mc25_load_rom(mc25 *m, const char *path)
{
	int n = load(path, m->rom, sizeof(m->rom));
	if (n > 0 && n <= 0x2000)
		memcpy(m->rom + 0x2000, m->rom, 0x2000);	/* 27C64 in a 27128 window */
	return n;
}

int
mc25_load_eerom(mc25 *m, const char *path)
{
	int n = load(path, m->eerom, sizeof(m->eerom));
	m->eerom_loaded = n > 0;
	return n;
}

void
mc25_power(mc25 *m, int on)
{
	if (on && !m->powered) {
		m->powered = 1;
		board_reset(m);
		event(m, MC25_EV_POWERON, 0);
	} else if (!on && m->powered) {
		m->powered = 0;
		event(m, MC25_EV_POWEROFF, 0);
	}
}

double mc25_time(const mc25 *m) { return (double)m->clk / MC25_HZ; }

void
mc25_set_input(mc25 *m, int w, int v)
{
	if (w >= 0 && w < MC25_IN_N)
		m->in[w] = !!v;
}

void mc25_key(mc25 *m, int code) { cu41_key(&m->cu, code); }

uint8_t
mc25_peek(mc25 *m, uint16_t a)
{
	if (a >= 0xc000)
		return m->nv[a & 0xff];
	return mem_read(m, a);
}

void
mc25_poke(mc25 *m, uint16_t a, uint8_t v)
{
	if (a < 0x4000)
		m->rom[a] = v;
	else
		mem_write(m, a, v);
}

void
mc25_breakpoint(mc25 *m, uint16_t a, int on)
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

static void
advance(mc25 *m, unsigned cyc)
{
	m->clk += (uint64_t)cyc * 8;
	if (m->clk >= m->next_tick) {
		m->cpu.int_line = 1;
		m->ticks++;
		m->next_tick = (uint64_t)((double)(m->ticks + 1) * TICK);
	}
	for (int d = 0; d < 2; d++)
		ccir_present(m, d);
	if ((double)(m->clk - m->wd_last) > m->wd_timeout_s * MC25_HZ) {
		event(m, MC25_EV_WDRESET, m->cpu.r[m->cpu.p]);
		board_reset(m);
	}
}

int
mc25_step(mc25 *m)
{
	if (!m->powered)
		return 0;
	uint16_t pc = m->cpu.r[m->cpu.p];
	m->trace[m->trace_pos++ % MC25_TRACE] = pc;
	unsigned cyc = cdp1802_step(&m->cpu);
	if (m->cpu.illegal)
		event(m, MC25_EV_ILLEGAL, pc);
	advance(m, cyc);
	return (int)cyc;
}

int
mc25_run(mc25 *m, double seconds)
{
	uint64_t end = m->clk + (uint64_t)(seconds * MC25_HZ);
	while (m->clk < end) {
		if (!m->powered) {
			m->clk = end;
			return MC25_STOP_OFF;
		}
		if (m->nbp && !m->cpu.idle) {
			uint16_t pc = m->cpu.r[m->cpu.p];
			if ((m->bp[pc >> 3] >> (pc & 7)) & 1 && !m->skip_bp) {
				m->skip_bp = 1;
				return MC25_STOP_BREAK;
			}
			m->skip_bp = 0;
		}
		mc25_step(m);
		if (m->watch_hit) {
			m->watch_hit = 0;
			return MC25_STOP_WATCH;
		}
	}
	return MC25_STOP_TIME;
}
