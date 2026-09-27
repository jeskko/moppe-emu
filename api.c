/*
 * Flat C API over r58.h for foreign-function use (Python ctypes).
 * Only plain integer/double/pointer types cross this boundary.
 */
#include <stdlib.h>
#include <string.h>

#include "r58.h"

r58 *
r58api_new(int card, int cu)
{
	r58 *m = calloc(1, sizeof(*m));
	if (m)
		r58_init(m, card, cu);
	return m;
}

void r58api_free(r58 *m) { free(m->rom1); free(m->aud_t); free(m->aud_v); free(m); }
int  r58api_load_rom1(r58 *m, const char *p) { return r58_load_rom1(m, p); }
int  r58api_load_rom(r58 *m, const char *p) { return r58_load_rom(m, p); }
int  r58api_load_nv(r58 *m, const char *p) { return r58_load_nv(m, p); }
int  r58api_save_nv(r58 *m, const char *p) { return r58_save_nv(m, p); }
int  r58api_run(r58 *m, double s) { return r58_run(m, s); }
double r58api_time(r58 *m) { return r58_time(m); }
void r58api_power(r58 *m, int on) { r58_power(m, on); }
int  r58api_powered(r58 *m) { return m->powered; }
void r58api_set_adc(r58 *m, int ch, int v) { r58_set_adc(m, ch, (uint8_t)v); }
void r58api_set_ptt(r58 *m, int v) { r58_set_ptt(m, v); }
void r58api_set_local(r58 *m, int v) { r58_set_local(m, v); }
void r58api_set_hook(r58 *m, int v) { r58_set_hook(m, v); }
void r58api_set_ccir(r58 *m, int v) { r58_set_ccir(m, v); }
void r58api_set_multiboard(r58 *m, int v) { r58_set_multiboard(m, (uint8_t)v); }
int  r58api_key(r58 *m, int k) { return r58_key(m, k); }
int  r58api_peek(r58 *m, int a) { return r58_peek(m, (uint16_t)a); }
void r58api_poke(r58 *m, int a, int v) { r58_poke(m, (uint16_t)a, (uint8_t)v); }
void r58api_set_wd_timeout(r58 *m, double s) { m->wd_timeout_s = s; }

void
r58api_read(r58 *m, int addr, int n, uint8_t *out)
{
	for (int i = 0; i < n; i++)
		out[i] = r58_peek(m, (uint16_t)(addr + i));
}

void
r58api_write(r58 *m, int addr, int n, const uint8_t *in)
{
	for (int i = 0; i < n; i++)
		r58_poke(m, (uint16_t)(addr + i), in[i]);
}

/* nv block (4096 bytes) direct access */
void
r58api_nv(r58 *m, uint8_t *out, const uint8_t *in)
{
	uint8_t *nv = m->card == R58_P8N ? m->nvplane : m->ram;
	if (out)
		memcpy(out, nv, R58_NV_SIZE);
	if (in)
		memcpy(nv, in, R58_NV_SIZE);
}

int
r58api_display(r58 *m, char *upper, char *lower)
{
	return r58_display_text(m, upper, 16, lower, 16);
}

/* raw display: CU53AN 128 segment bits as 16 bytes; CU58AF 40 bytes */
int
r58api_display_raw(r58 *m, uint8_t *out)
{
	if (m->cu == R58_CU53AN) {
		for (int i = 0; i < 16; i++) {
			uint8_t b = 0;
			for (int k = 0; k < 8; k++)
				b |= cu53an_segment(&m->cu53, i * 8 + k) << k;
			out[i] = b;
		}
		out[16] = m->cu53.latch;
		return 17;
	}
	memcpy(out, m->cu58.lcd_ram, 40);
	out[40] = m->cu58.led.latch;
	return 41;
}

/* synth: rx_r rx_n rx_a tx_r tx_n tx_a ctrl ext_a ext_b rx_loads tx_loads ctrl_loads */
void
r58api_synth(r58 *m, uint32_t *o)
{
	r58_synth *s = &m->synth;
	o[0] = s->rx_r; o[1] = s->rx_n; o[2] = s->rx_a;
	o[3] = s->tx_r; o[4] = s->tx_n; o[5] = s->tx_a;
	o[6] = s->ctrl; o[7] = s->ext_a; o[8] = s->ext_b;
	o[9] = s->rx_loads; o[10] = s->tx_loads; o[11] = s->ctrl_loads;
}

/* latches: out0 out1 out2 da_rfc da_txpwr pio_a_pins pio_b_pins csmem */
void
r58api_latches(r58 *m, uint8_t *o)
{
	o[0] = m->out0; o[1] = m->out1; o[2] = m->out2;
	o[3] = m->da_rfc; o[4] = m->da_txpwr;
	o[5] = pio_pins(&m->pio, 0); o[6] = pio_pins(&m->pio, 1);
	o[7] = m->csmem;
}

/* timer 1 (tone pin): mode, count register, out */
void
r58api_pit(r58 *m, int n, uint32_t *o)
{
	pit_counter *c = &m->pit.c[n & 3];
	o[0] = c->mode; o[1] = c->cr; o[2] = c->out; o[3] = c->null;
}

/* cpu: af bc de hl ix iy sp pc af' bc' de' hl' i r iff1 im halted */
void
r58api_cpu(r58 *m, uint32_t *o)
{
	z80 *z = &m->cpu;
	o[0] = (z->a << 8) | z->f; o[1] = (z->b << 8) | z->c;
	o[2] = (z->d << 8) | z->e; o[3] = (z->h << 8) | z->l;
	o[4] = z->ix; o[5] = z->iy; o[6] = z->sp; o[7] = z->pc;
	o[8] = (z->a_ << 8) | z->f_; o[9] = (z->b_ << 8) | z->c_;
	o[10] = (z->d_ << 8) | z->e_; o[11] = (z->h_ << 8) | z->l_;
	o[12] = z->i; o[13] = z->r; o[14] = z->iff1; o[15] = z->im;
	o[16] = z->halted;
}

unsigned long long r58api_instructions(r58 *m) { return m->instructions; }

int
r58api_next_event(r58 *m, double *at, int *type, int *arg)
{
	r58_event e;
	if (!r58_next_event(m, &e))
		return 0;
	*at = e.at / R58_XTAL_HZ;
	*type = e.type;
	*arg = e.arg;
	return 1;
}

void r58api_bp(r58 *m, int addr, int on) { m->bp[addr & 0xffff] = !!on; }
void r58api_wp(r58 *m, int addr, int on) { m->wp[addr & 0xffff] = !!on; }
int  r58api_stop_addr(r58 *m) { return m->stop_addr; }

/* last n PCs, oldest first */
int
r58api_trace(r58 *m, uint16_t *out, int n)
{
	if (n > R58_TRACE)
		n = R58_TRACE;
	for (int i = 0; i < n; i++)
		out[i] = m->trace[(m->trace_pos - n + i) % R58_TRACE];
	return n;
}

void
r58api_serial_rx(r58 *m, int chan, const uint8_t *buf, int n)
{
	r58_serial_rx(m, chan, buf, n);
}

void
r58api_modem_rx(r58 *m, const uint8_t *buf, int n)
{
	r58_modem_rx(m, buf, n);
}

/* CU58AF extra inputs: speaker button, dark (LDR) */
void
r58api_cu58_buttons(r58 *m, int speaker, int dark, int power)
{
	m->cu58.speaker = !!speaker;
	m->cu58.dark = !!dark;
	m->cu58.power = !!power;
}

void r58api_audio_capture(r58 *m, unsigned cap) { r58_audio_capture(m, cap); }

unsigned
r58api_audio_take(r58 *m, uint64_t *t, uint8_t *v, unsigned max)
{
	return r58_audio_take(m, t, v, max);
}

double r58api_xtal(void) { return R58_XTAL_HZ; }
void r58api_set_m1_wait(r58 *m, int w) { m->m1_wait = w; }
void r58api_step(r58 *m, int n) { while (n-- > 0) r58_step(m); }
