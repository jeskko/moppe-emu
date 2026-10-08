/*
 * Flat C API over mdr150.h for Python ctypes (libmdr150.so).
 */
#include <stdlib.h>
#include <string.h>

#include "mdr150.h"

mdr150 *
mdrapi_new(void)
{
	mdr150 *r = calloc(1, sizeof(*r));
	if (r)
		mdr150_init(r);
	return r;
}

void   mdrapi_free(mdr150 *r) { mdr150_free(r); free(r); }
int    mdrapi_load_flash(mdr150 *r, const uint8_t *img, int n) { return mdr150_load_flash(r, img, n); }
void   mdrapi_power(mdr150 *r, int on) { mdr150_power(r, on); }
int    mdrapi_run(mdr150 *r, double s) { return mdr150_run(r, s); }
int    mdrapi_step(mdr150 *r) { return mdr150_step(r); }
double mdrapi_time(mdr150 *r) { return mdr150_time(r); }
double mdrapi_fsys(mdr150 *r) { return hc16z1_fsys(&r->chip); }
double mdrapi_vco_hz(mdr150 *r) { return mdr150_vco_hz(r); }
double mdrapi_sci_baud(mdr150 *r) { return hc16z1_sci_baud(&r->chip); }
int    mdrapi_oplen(mdr150 *r, int a) { return cpu16_oplen(&r->chip.cpu, (uint32_t)a); }

void   mdrapi_log_exc(mdr150 *r, int on) { r->log_exc = on != 0; }
void   mdrapi_set_rssi(mdr150 *r, int v) { r->rssi = v; }
void   mdrapi_set_busy(mdr150 *r, int v) { r->busy = v != 0; }
void   mdrapi_set_levels(mdr150 *r, int audio, int noise) { r->audio_level = audio; r->noise_level = noise; }
void   mdrapi_set_rx_audio(mdr150 *r, const float *x, int n, double rate) { mdr150_set_rx_audio(r, x, n, rate); }
int    mdrapi_rx_audio_left(mdr150 *r) { return r->rx_audio != NULL; }

/* serial: host lines of port p (0 = port 1, 1 = port 2) */
void   mdrapi_set_lines(mdr150 *r, int p, int dtr, int rts) { r->port[p & 1].dtr = dtr != 0; r->port[p & 1].rts = rts != 0; }
void   mdrapi_serial_in(mdr150 *r, int p, int b) { mdr150_serial_in(r, p, (uint8_t)b); }
int    mdrapi_serial_pending(mdr150 *r) { return hc16z1_sci_rx_pending(&r->chip); }
int    mdrapi_ssel(mdr150 *r) { return r->ssel; }

/* bytes the radio sent on port p, up to n; returns the count */
int
mdrapi_serial_out(mdr150 *r, int p, uint8_t *out, int n)
{
	mdr150_port *q = &r->port[p & 1];
	int k = 0;
	while (k < n && q->out_tail != q->out_head) {
		out[k++] = q->out[q->out_tail];
		q->out_tail = (q->out_tail + 1) % (int)sizeof(q->out);
	}
	return k;
}

/* radio state: 4094 outputs, port F, tx keyed, power level, LEDs, N*64+A, R */
void
mdrapi_radio(mdr150 *r, int *out)
{
	out[0] = r->sr_out;
	out[1] = r->pf;
	out[2] = r->tx_keyed;
	out[3] = r->power_level;
	out[4] = r->leds;
	out[5] = (int)r->pll_na;
	out[6] = (int)r->pll_r;
}

/* TX audio: PWMA duty changes while keyed; take and clear */
int mdrapi_pwm_count(mdr150 *r) { return r->pwm_n; }

void
mdrapi_pwm_take(mdr150 *r, double *at, int *duty)
{
	for (int i = 0; i < r->pwm_n; i++) {
		at[i] = (double)r->pwm_at[i] / 16777216.0;
		duty[i] = r->pwm_duty[i];
	}
	r->pwm_n = 0;
}

void
mdrapi_read(mdr150 *r, int addr, int n, uint8_t *out)
{
	for (int i = 0; i < n; i++)
		out[i] = hc16z1_peek(&r->chip, (uint32_t)(addr + i) & 0xFFFFF);
}

void
mdrapi_write(mdr150 *r, int addr, int n, const uint8_t *in)
{
	for (int i = 0; i < n; i++)
		hc16z1_poke(&r->chip, (uint32_t)(addr + i) & 0xFFFFF, in[i]);
}

/* flash image (128 KB): out receives it, in replaces it; dirty flag */
int
mdrapi_flash(mdr150 *r, uint8_t *out, const uint8_t *in)
{
	int d = r->flash_dirty;
	if (out)
		memcpy(out, r->flash, sizeof(r->flash));
	if (in)
		memcpy(r->flash, in, sizeof(r->flash));
	r->flash_dirty = 0;
	return d;
}

/* registers: d e ix iy iz sp pc ccr xk yk zk ek sk pk waiting hr ir */
void
mdrapi_cpu(mdr150 *r, int *out)
{
	cpu16 *c = &r->chip.cpu;
	out[0] = c->d; out[1] = c->e; out[2] = c->r[0]; out[3] = c->r[1]; out[4] = c->r[2];
	out[5] = c->sp; out[6] = c->pc; out[7] = cpu16_ccr(c);
	out[8] = c->k[0]; out[9] = c->k[1]; out[10] = c->k[2]; out[11] = c->ek; out[12] = c->sk;
	out[13] = c->pk; out[14] = c->waiting; out[15] = c->hr; out[16] = c->ir;
}

void
mdrapi_breakpoint(mdr150 *r, int a, int on)
{
	uint32_t x = (uint32_t)a & 0xFFFFF;
	if (on)
		r->bp[x >> 3] |= (uint8_t)(1 << (x & 7));
	else
		r->bp[x >> 3] &= (uint8_t)~(1 << (x & 7));
}

int
mdrapi_event(mdr150 *r, double *at, int *type, int *arg)
{
	mdr150_event *e;
	if (r->ev_tail == r->ev_head)
		return 0;
	e = &r->ev[r->ev_tail];
	r->ev_tail = (r->ev_tail + 1) % 4096;
	*at = e->at;
	*type = e->type;
	*arg = e->arg;
	return 1;
}

int
mdrapi_trace(mdr150 *r, int n, int *out)
{
	if (n > 256)
		n = 256;
	for (int i = 0; i < n; i++)
		out[i] = (int)r->trace[(r->trace_n - n + i) & 255];
	return n;
}

/* chip internals for tests: unmapped access count and last address */
int mdrapi_unmapped(mdr150 *r, int *addr) { *addr = (int)r->chip.unmapped_addr; return (int)r->chip.unmapped_count; }
