/*
 * Flat C API over mc25.h for Python ctypes (libmc25.so).
 */
#include <stdlib.h>
#include <string.h>

#include "mc25.h"

mc25 *
mc25api_new(void)
{
	mc25 *m = calloc(1, sizeof(*m));
	if (m)
		mc25_init(m);
	return m;
}

void   mc25api_free(mc25 *m) { free(m->q_t); free(m->q_v); free(m); }
int    mc25api_load_rom(mc25 *m, const char *p) { return mc25_load_rom(m, p); }
int    mc25api_load_eerom(mc25 *m, const char *p) { return mc25_load_eerom(m, p); }
void   mc25api_power(mc25 *m, int on) { mc25_power(m, on); }
int    mc25api_powered(mc25 *m) { return m->powered; }
int    mc25api_run(mc25 *m, double s) { return mc25_run(m, s); }
int    mc25api_step(mc25 *m) { return mc25_step(m); }
double mc25api_time(mc25 *m) { return mc25_time(m); }
void   mc25api_set_input(mc25 *m, int w, int v) { mc25_set_input(m, w, v); }
void   mc25api_key(mc25 *m, int code) { mc25_key(m, code); }
void   mc25api_set_wd_timeout(mc25 *m, double s) { m->wd_timeout_s = s; }
int    mc25api_peek(mc25 *m, int a) { return mc25_peek(m, (uint16_t)a); }
void   mc25api_poke(mc25 *m, int a, int v) { mc25_poke(m, (uint16_t)a, (uint8_t)v); }
void   mc25api_breakpoint(mc25 *m, int a, int on) { mc25_breakpoint(m, (uint16_t)a, on); }
void   mc25api_watch(mc25 *m, int lo, int hi) { m->watch_lo = (uint16_t)lo; m->watch_hi = (uint16_t)hi; }
int    mc25api_out(mc25 *m, int n) { return m->out[n & 7]; }
int    mc25api_ticks(mc25 *m) { return (int)m->ticks; }
unsigned mc25api_dpy_bytes(mc25 *m) { return m->cu.bytes; }
int    mc25api_dpy_commands(mc25 *m) { return (int)m->cu.commands; }

void
mc25api_ccir_rx(mc25 *m, int d, int n, const uint8_t *digits)
{
	mc25_ccir_rx(m, d, digits, n);
}

void
mc25api_serial_rx(mc25 *m, int n, const uint8_t *bytes)
{
	mc25_serial_rx(m, bytes, n);
}

int mc25api_serial_pending(mc25 *m) { return m->ser_n; }

void
mc25api_read(mc25 *m, int addr, int n, uint8_t *out)
{
	for (int i = 0; i < n; i++)
		out[i] = mc25_peek(m, (uint16_t)(addr + i));
}

void
mc25api_write(mc25 *m, int addr, int n, const uint8_t *in)
{
	for (int i = 0; i < n; i++)
		mc25_poke(m, (uint16_t)(addr + i), in[i]);
}

/* battery RAM 1024 bytes, then X2212 256 nibbles */
void
mc25api_nv(mc25 *m, uint8_t *out, const uint8_t *in)
{
	if (out) {
		memcpy(out, m->ram, sizeof(m->ram));
		memcpy(out + sizeof(m->ram), m->nv, sizeof(m->nv));
	}
	if (in) {
		memcpy(m->ram, in, sizeof(m->ram));
		memcpy(m->nv, in + sizeof(m->ram), sizeof(m->nv));
	}
}

/* 16 display cells (raw codes), returns decimal-point mask */
int
mc25api_display(mc25 *m, uint8_t *cells)
{
	memcpy(cells, m->cu.cell, CU41_CELLS);
	return m->cu.dp;
}

void
mc25api_cpu(mc25 *m, int *out)
{
	for (int i = 0; i < 16; i++)
		out[i] = m->cpu.r[i];
	out[16] = m->cpu.p; out[17] = m->cpu.x; out[18] = m->cpu.d;
	out[19] = m->cpu.df; out[20] = m->cpu.t; out[21] = m->cpu.ie;
	out[22] = m->cpu.q; out[23] = m->cpu.idle;
}

int
mc25api_event(mc25 *m, double *at, int *type, int *arg)
{
	mc25_event e;
	if (!mc25_event_pop(m, &e))
		return 0;
	*at = (double)e.at / MC25_HZ;
	*type = e.type;
	*arg = e.arg;
	return 1;
}

int
mc25api_trace(mc25 *m, int n, int *out)
{
	if (n > MC25_TRACE)
		n = MC25_TRACE;
	for (int i = 0; i < n; i++)
		out[i] = m->trace[(m->trace_pos - n + i) % MC25_TRACE];
	return n;
}

/* Q edges since the last call (times in seconds, levels) */
int
mc25api_q_take(mc25 *m, int max, double *t, uint8_t *v)
{
	int n = (int)m->q_n < max ? (int)m->q_n : max;
	for (int i = 0; i < n; i++) {
		t[i] = (double)m->q_t[i] / MC25_HZ;
		v[i] = m->q_v[i];
	}
	memmove(m->q_t, m->q_t + n, (m->q_n - (unsigned)n) * sizeof(*m->q_t));
	memmove(m->q_v, m->q_v + n, m->q_n - (unsigned)n);
	m->q_n -= (unsigned)n;
	return n;
}

int mc25api_q_level(mc25 *m) { return m->cpu.q; }
