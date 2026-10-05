/*
 * Flat C API over r40.h for Python ctypes (libr40.so).
 */
#include <stdlib.h>
#include <string.h>

#include "r40.h"

r40 *
r40api_new(void)
{
	r40 *m = calloc(1, sizeof(*m));
	if (m)
		r40_init(m);
	return m;
}

void   r40api_free(r40 *m) { r40_free(m); free(m); }
int    r40api_load_rom(r40 *m, const char *p) { return r40_load_rom(m, p); }
void   r40api_power(r40 *m, int on) { r40_power(m, on); }
int    r40api_powered(r40 *m) { return m->powered; }
int    r40api_run(r40 *m, double s) { return r40_run(m, s); }
int    r40api_step(r40 *m) { return r40_step(m); }
double r40api_time(r40 *m) { return r40_time(m); }
double r40api_cpu_hz(r40 *m) { (void)m; return R40_HZ; }
void   r40api_set_adc(r40 *m, int ch, int v) { m->an[ch & 7] = v; }
int    r40api_adc(r40 *m, int ch) { return m->an[ch & 7]; }
void   r40api_key(r40 *m, int row, int col, int down) { r40_key(m, row, col, down); }
void   r40api_set_onoff(r40 *m, int down) { r40_set_onoff(m, down); }
void   r40api_set_hook(r40 *m, int offhook) { r40_set_hook(m, offhook); }
void   r40api_set_ptt(r40 *m, int down) { r40_set_ptt(m, down); }
void   r40api_set_service_head(r40 *m, int on) { m->service_head = on != 0; }
void   r40api_set_head_cu43(r40 *m, int on) { m->head_cu43 = on != 0; }
void   r40api_modem_rx(r40 *m, int b) { r40_modem_rx(m, (uint8_t)b); }
void   r40api_sci_rx(r40 *m, int b) { h8532_sci_rx(&m->chip, (uint8_t)b); }
int    r40api_peek(r40 *m, int a) { return r40_peek(m, (uint32_t)a); }
void   r40api_poke(r40 *m, int a, int v) { r40_poke(m, (uint32_t)a, (uint8_t)v); }
void   r40api_breakpoint(r40 *m, int a, int on) { r40_breakpoint(m, (uint32_t)a, on); }
void   r40api_watch(r40 *m, int lo, int hi) { m->watch_lo = (uint32_t)lo; m->watch_hi = (uint32_t)hi; }
int    r40api_watch_addr(r40 *m) { return (int)m->watch_addr; }
int    r40api_out(r40 *m, int n) { return n ? m->out1 : m->out0; }
int    r40api_sreg(r40 *m, int n) { return m->sreg[n % 3]; }
int    r40api_dac(r40 *m, int n) { return m->dac[n & 3]; }
int    r40api_pcf(r40 *m, int n) { return m->pcf[n % 3]; }
int    r40api_oplen(r40 *m, int a) { return h8500_oplen(&m->chip.cpu, (uint32_t)a); }
double r40api_sci_baud(r40 *m) { return h8532_sci_baud(&m->chip); }

void
r40api_read(r40 *m, int addr, int n, uint8_t *out)
{
	for (int i = 0; i < n; i++)
		out[i] = r40_peek(m, (uint32_t)(addr + i));
}

void
r40api_write(r40 *m, int addr, int n, const uint8_t *in)
{
	for (int i = 0; i < n; i++)
		r40_poke(m, (uint32_t)(addr + i), in[i]);
}

/* battery-backed SRAM image (32 KB) */
int
r40api_nv(r40 *m, uint8_t *out, const uint8_t *in)
{
	if (out)
		memcpy(out, m->nv, sizeof(m->nv));
	if (in)
		memcpy(m->nv, in, sizeof(m->nv));
	return (int)sizeof(m->nv);
}

/* service-key EEPROM (256 bytes) */
void
r40api_eeprom(r40 *m, uint8_t *out, const uint8_t *in)
{
	if (out)
		memcpy(out, m->eeprom, sizeof(m->eeprom));
	if (in)
		memcpy(m->eeprom, in, sizeof(m->eeprom));
}

/* LCD driver RAM: 4 devices x 4 banks x 40 columns */
void
r40api_lcd(r40 *m, uint8_t *out)
{
	memcpy(out, m->lcd, sizeof(m->lcd));
}

/* which 0 = RX (SRE), 1 = TX (STE): r, sw, n, a, loads; returns Hz */
double
r40api_pll(r40 *m, int which, unsigned *out)
{
	r40_pll *p = &m->pll[which & 1];
	out[0] = p->r; out[1] = p->sw; out[2] = p->n; out[3] = p->a; out[4] = p->loads;
	return r40_pll_hz(m, which & 1);
}

/* registers: r0..r7 pc sr cp dp ep tp br sleeping */
void
r40api_cpu(r40 *m, int *out)
{
	h8500 *c = &m->chip.cpu;
	for (int i = 0; i < 8; i++)
		out[i] = c->r[i];
	out[8] = c->pc; out[9] = c->sr; out[10] = c->cp; out[11] = c->dp;
	out[12] = c->ep; out[13] = c->tp; out[14] = c->br; out[15] = c->sleeping;
}

int
r40api_event(r40 *m, double *at, int *type, int *arg)
{
	r40_event e;
	if (!r40_event_pop(m, &e))
		return 0;
	*at = (double)e.at / R40_HZ;
	*type = e.type;
	*arg = e.arg;
	return 1;
}

int
r40api_trace(r40 *m, int n, int *out)
{
	if (n > R40_TRACE)
		n = R40_TRACE;
	for (int i = 0; i < n; i++)
		out[i] = (int)m->trace[(m->trace_pos + R40_TRACE - n + i) % R40_TRACE];
	return n;
}

/* coverage map of the ROM (R40_COV bytes, flags in r40.h): the first
 * call starts recording; out, if given, receives the map so far */
int
r40api_coverage(r40 *m, uint8_t *out)
{
	if (!m->cov && !(m->cov = calloc(1, R40_COV)))
		return -1;
	if (out)
		memcpy(out, m->cov, R40_COV);
	return R40_COV;
}

/* I2C log: total count; entry k (k < total, the last R40_I2CLOG kept) */
int r40api_i2c_count(r40 *m) { return (int)m->i2clog_n; }

int
r40api_i2c_entry(r40 *m, int k, double *at, uint8_t *out)
{
	r40_i2clog *l;
	int n;
	if (k < 0 || (unsigned)k >= m->i2clog_n || m->i2clog_n - (unsigned)k > R40_I2CLOG)
		return -1;
	l = &m->i2clog[(unsigned)k % R40_I2CLOG];
	*at = (double)l->at / R40_HZ;
	n = l->n < R40_I2CLOG_BYTES ? l->n : R40_I2CLOG_BYTES;
	memcpy(out, l->b, (size_t)n);
	return l->n;
}
