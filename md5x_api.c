/*
 * Flat C API over md5x.h for Python ctypes (libmd5x.so).
 */
#include <stdlib.h>
#include <string.h>

#include "md5x.h"

md5x *
md5xapi_new(int model, int cu, int ram_size)
{
	md5x *m = calloc(1, sizeof(*m));
	if (m)
		md5x_init(m, model, cu, (unsigned)ram_size);
	return m;
}

void   md5xapi_free(md5x *m) { free(m->aud_t); free(m->aud_v); free(m->iolog); free(m); }
int    md5xapi_load_rom(md5x *m, const char *p) { return md5x_load_rom(m, p); }
void   md5xapi_power(md5x *m, int on) { md5x_power(m, on); }
int    md5xapi_powered(md5x *m) { return m->powered; }
int    md5xapi_run(md5x *m, double s) { return md5x_run(m, s); }
int    md5xapi_step(md5x *m) { return md5x_step(m); }
double md5xapi_time(md5x *m) { return md5x_time(m); }
double md5xapi_cpu_hz(md5x *m) { return m->cpu_hz; }
void   md5xapi_set_input(md5x *m, int w, int v) { md5x_set_input(m, w, v); }
void   md5xapi_key(md5x *m, int code) { md5x_key(m, code); }
void   md5xapi_set_squelch(md5x *m, int open) { m->squelch_open = open; }
void   md5xapi_set_rx_tone(md5x *m, double hz) { m->rx_tone_hz = hz; }
void   md5xapi_set_adc(md5x *m, int ch, int v) { m->adc[ch & 7] = (uint8_t)v; }
int    md5xapi_dac(md5x *m) { return m->dac; }
void   md5xapi_set_wd_timeout(md5x *m, double s) { m->wd_timeout_s = s; }
void   md5xapi_modem_rx(md5x *m, int b) { md5x_modem_rx(m, (uint8_t)b); }
int    md5xapi_peek(md5x *m, int a) { return md5x_peek(m, (uint16_t)a); }
void   md5xapi_poke(md5x *m, int a, int v) { md5x_poke(m, (uint16_t)a, (uint8_t)v); }
void   md5xapi_breakpoint(md5x *m, int a, int on) { md5x_breakpoint(m, (uint16_t)a, on); }
void   md5xapi_watch(md5x *m, int lo, int hi) { m->watch_lo = (uint16_t)lo; m->watch_hi = (uint16_t)hi; }
int    md5xapi_watch_addr(md5x *m) { return m->watch_addr; }
int    md5xapi_latch(md5x *m, int n) { return m->latch[n & 7]; }
int    md5xapi_leds(md5x *m) { return m->hs.latch; }
unsigned md5xapi_lcd_frames(md5x *m) { return m->hs.frames; }
int    md5xapi_modem_ticks(md5x *m) { return (int)m->modem.ticks; }

void
md5xapi_read(md5x *m, int addr, int n, uint8_t *out)
{
	for (int i = 0; i < n; i++)
		out[i] = md5x_peek(m, (uint16_t)(addr + i));
}

void
md5xapi_write(md5x *m, int addr, int n, const uint8_t *in)
{
	for (int i = 0; i < n; i++)
		md5x_poke(m, (uint16_t)(addr + i), in[i]);
}

/* battery RAM image */
int
md5xapi_ram(md5x *m, uint8_t *out, const uint8_t *in)
{
	if (out)
		memcpy(out, m->ram, m->ram_size);
	if (in)
		memcpy(m->ram, in, m->ram_size);
	return (int)m->ram_size;
}

/* 128 segment bits in firmware order (bit p of the 16 bytes) */
void
md5xapi_segments(md5x *m, uint8_t *out)
{
	for (int i = 0; i < 16; i++) {
		uint8_t b = 0;
		for (int k = 0; k < 8; k++)
			b |= (uint8_t)(cu53an_segment(&m->hs, i * 8 + k) << k);
		out[i] = b;
	}
}

/* r[16], p, x, d, df, t, ie, q, idle */
void
md5xapi_cpu(md5x *m, int *out)
{
	for (int i = 0; i < 16; i++)
		out[i] = m->cpu.r[i];
	out[16] = m->cpu.p;
	out[17] = m->cpu.x;
	out[18] = m->cpu.d;
	out[19] = m->cpu.df;
	out[20] = m->cpu.t;
	out[21] = m->cpu.ie;
	out[22] = m->cpu.q;
	out[23] = m->cpu.idle;
}

void
md5xapi_set_reg(md5x *m, int r, int v)
{
	m->cpu.r[r & 15] = (uint16_t)v;
}

/* n, a, div, sw, off_n, off_a, off_sw, loads, off_loads, last_nbits */
void
md5xapi_synth(md5x *m, unsigned *out)
{
	md5x_synth *s = &m->synth;
	out[0] = s->n; out[1] = s->a; out[2] = s->div; out[3] = s->sw;
	out[4] = s->off_n; out[5] = s->off_a; out[6] = s->off_sw;
	out[7] = s->loads; out[8] = s->off_loads; out[9] = (unsigned)s->last_nbits;
}

int
md5xapi_event(md5x *m, double *at, int *type, int *arg)
{
	md5x_event e;
	if (!md5x_event_pop(m, &e))
		return 0;
	*at = (double)e.at / m->cpu_hz;
	*type = e.type;
	*arg = e.arg;
	return 1;
}

int
md5xapi_trace(md5x *m, int n, int *out)
{
	if (n > MD5X_TRACE)
		n = MD5X_TRACE;
	for (int i = 0; i < n; i++)
		out[i] = m->trace[(m->trace_pos - n + i) % MD5X_TRACE];
	return n;
}

void
md5xapi_audio_capture(md5x *m, int on, int bit)
{
	if (bit >= 0)
		m->aud_bit = bit;
	md5x_audio_capture(m, on);
}

/* copies up to max edges: times in seconds and levels; returns count */
int
md5xapi_audio(md5x *m, int max, double *t, uint8_t *v)
{
	int n = (int)m->aud_n < max ? (int)m->aud_n : max;
	for (int i = 0; i < n; i++) {
		t[i] = (double)m->aud_t[i] / m->cpu_hz;
		v[i] = m->aud_v[i];
	}
	return n;
}

int md5xapi_audio_count(md5x *m) { return (int)m->aud_n; }

/* ROM coverage bitmap (4096 bytes, bit a&7 of byte a>>3) */
void md5xapi_coverage(md5x *m, uint8_t *out, int clear)
{
	memcpy(out, m->cov, sizeof(m->cov));
	if (clear)
		memset(m->cov, 0, sizeof(m->cov));
}

void md5xapi_iolog(md5x *m, int on, int memw) { md5x_iolog(m, on, memw); }

/* oldest-first copy of the I/O log: at (s), pc, kind, n, addr, val;
 * returns count and clears the log */
int
md5xapi_iolog_take(md5x *m, int max, double *at, int *out)
{
	if (!m->iolog)
		return 0;
	unsigned n = m->io_n < MD5X_IOLOG ? m->io_n : MD5X_IOLOG;
	unsigned first = m->io_n - n;
	if ((int)n > max)
		n = (unsigned)max;
	for (unsigned i = 0; i < n; i++) {
		md5x_io *e = &m->iolog[(first + i) % MD5X_IOLOG];
		at[i] = (double)e->at / m->cpu_hz;
		out[i * 5 + 0] = e->pc;
		out[i * 5 + 1] = e->kind;
		out[i * 5 + 2] = e->n;
		out[i * 5 + 3] = e->addr;
		out[i * 5 + 4] = e->val;
	}
	m->io_n = 0;
	return (int)n;
}

/* 93C06 contents, 16 words; out/in may be NULL */
void
md5xapi_eeprom(md5x *m, uint16_t *out, const uint16_t *in)
{
	if (out)
		memcpy(out, m->ee.word, sizeof(m->ee.word));
	if (in)
		memcpy(m->ee.word, in, sizeof(m->ee.word));
}

void md5xapi_progmodule(md5x *m, const uint8_t *nib, int n) { md5x_progmodule(m, nib, n); }
