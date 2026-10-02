/*
 * Flat C API over tmx1.h for Python ctypes (libtmx1.so).
 */
#include <stdlib.h>
#include <string.h>

#include "tmx1.h"

tmx1 *
tmx1api_new(int handset)
{
	tmx1 *m = calloc(1, sizeof(*m));
	if (m)
		tmx1_init(m, handset);
	return m;
}

void   tmx1api_free(tmx1 *m) { free(m->aud_t); free(m->aud_v); free(m); }
int    tmx1api_load_rom(tmx1 *m, const char *p) { return tmx1_load_rom(m, p); }
int    tmx1api_load_hs_rom(tmx1 *m, const char *p) { return tmx1_load_hs_rom(m, p); }
void   tmx1api_power(tmx1 *m, int on) { tmx1_power(m, on); }
int    tmx1api_powered(tmx1 *m) { return m->powered; }
int    tmx1api_run(tmx1 *m, double s) { return tmx1_run(m, s); }
int    tmx1api_step(tmx1 *m) { return tmx1_step(m); }
double tmx1api_time(tmx1 *m) { return tmx1_time(m); }
double tmx1api_cpu_hz(tmx1 *m) { (void)m; return TMX1_HZ; }
void   tmx1api_set_adc(tmx1 *m, int ch, int v) { m->an[ch & 7] = (uint8_t)v; }
int    tmx1api_adc(tmx1 *m, int ch) { return m->an[ch & 7]; }
void   tmx1api_set_ptt(tmx1 *m, int on) { m->ptt = on != 0; }
void   tmx1api_key(tmx1 *m, int scancode, int down) { tmx1hs_key(&m->hs, scancode, down); }
void   tmx1api_set_hook(tmx1 *m, int offhook) { tmx1hs_set_hook(&m->hs, offhook); }
void   tmx1api_power_key(tmx1 *m, int down) { tmx1_power_key(m, down); }
void   tmx1api_mbus_send(tmx1 *m, int b) { tmx1_mbus_send(m, (uint8_t)b); }
int    tmx1api_mbus_pending(tmx1 *m) { return (int)m->hq_n + (m->host_t0 != 0); }
void   tmx1api_modem_rx(tmx1 *m, int b) { tmx1_modem_rx(m, (uint8_t)b); }
void   tmx1api_set_if_hz(tmx1 *m, double hz) { m->if_hz = hz; }
void   tmx1api_set_wd(tmx1 *m, double nmi_s, double off_s) { m->wd_nmi_s = nmi_s; m->wd_off_s = off_s; }
int    tmx1api_peek(tmx1 *m, int a) { return tmx1_peek(m, (uint16_t)a); }
void   tmx1api_poke(tmx1 *m, int a, int v) { tmx1_poke(m, (uint16_t)a, (uint8_t)v); }
int    tmx1api_hs_peek(tmx1 *m, int a) { return upd7810_peek(&m->hs.cpu, (uint16_t)a); }
void   tmx1api_breakpoint(tmx1 *m, int a, int on) { tmx1_breakpoint(m, (uint16_t)a, on); }
void   tmx1api_watch(tmx1 *m, int lo, int hi) { m->watch_lo = (uint16_t)lo; m->watch_hi = (uint16_t)hi; }
int    tmx1api_watch_addr(tmx1 *m) { return m->watch_addr; }
int    tmx1api_hs_type(tmx1 *m) { return m->hs.type; }
int    tmx1api_hs_icons(tmx1 *m) { return tmx1hs_icons(&m->hs); }
int    tmx1api_hs_leds(tmx1 *m) { return tmx1hs_leds(&m->hs); }
int    tmx1api_hs_dtmf(tmx1 *m) { return m->hs.dtmf; }
int    tmx1api_hs_port(tmx1 *m, int p) { return p == 0 ? m->hs.pa : p == 1 ? m->hs.pb : m->hs.pc; }
int    tmx1api_hs_hsic(tmx1 *m, int n) { return m->hs.hsic[n & 3]; }
int    tmx1api_port(tmx1 *m, int p) { return p == 0 ? m->pa : p == 1 ? m->pb : m->pc; }
int    tmx1api_dev(tmx1 *m) { return m->dev; }
int    tmx1api_lfu(tmx1 *m, int n) { return m->lfu[n & 3]; }
int    tmx1api_dac(tmx1 *m, int n) { return m->dac[n & 3]; }
int    tmx1api_out2(tmx1 *m) { return m->out2; }
int    tmx1api_oplen(tmx1 *m, int a) { return upd7810_oplen(&m->cpu, (uint16_t)a); }

void
tmx1api_read(tmx1 *m, int addr, int n, uint8_t *out)
{
	for (int i = 0; i < n; i++)
		out[i] = tmx1_peek(m, (uint16_t)(addr + i));
}

void
tmx1api_write(tmx1 *m, int addr, int n, const uint8_t *in)
{
	for (int i = 0; i < n; i++)
		tmx1_poke(m, (uint16_t)(addr + i), in[i]);
}

/* battery-backed RAM image (8 KB) */
int
tmx1api_ram(tmx1 *m, uint8_t *out, const uint8_t *in)
{
	if (out)
		memcpy(out, m->ram, sizeof(m->ram));
	if (in)
		memcpy(m->ram, in, sizeof(m->ram));
	return (int)sizeof(m->ram);
}

/* mid[9], bot[9], top[3] */
void
tmx1api_hs_text(tmx1 *m, char *mid, char *bot, char *top)
{
	tmx1hs_text(&m->hs, mid, bot, top);
}

/* the two uPD7228s' display RAM and character codes: 4 x 128 bytes */
void
tmx1api_hs_lcd(tmx1 *m, uint8_t *out)
{
	memcpy(out, m->hs.lcd[0].ram, 128);
	memcpy(out + 128, m->hs.lcd[0].chr, 128);
	memcpy(out + 256, m->hs.lcd[1].ram, 128);
	memcpy(out + 384, m->hs.lcd[1].chr, 128);
}

/* which 0 = rx, 1 = tx: r, sw, d, loads; returns VCO Hz */
double
tmx1api_pll(tmx1 *m, int which, unsigned *out)
{
	tmx1_pll *p = which ? &m->tx : &m->rx;
	out[0] = p->r; out[1] = p->sw; out[2] = p->d; out[3] = p->loads;
	return tmx1_pll_hz(m, p);
}

/* registers: v a b c d e h l ea sp pc psw ie halt (radio or handset) */
void
tmx1api_cpu(tmx1 *m, int hs, int *out)
{
	upd7810 *c = hs ? &m->hs.cpu : &m->cpu;
	out[0] = c->v; out[1] = c->a; out[2] = c->b; out[3] = c->c;
	out[4] = c->d; out[5] = c->e; out[6] = c->h; out[7] = c->l;
	out[8] = c->ea; out[9] = c->sp; out[10] = c->pc; out[11] = c->psw;
	out[12] = c->ie; out[13] = c->halt;
	out[14] = c->mkl | c->mkh << 8; out[15] = (int)c->irr;
}

int
tmx1api_event(tmx1 *m, double *at, int *type, int *arg)
{
	tmx1_event e;
	if (!tmx1_event_pop(m, &e))
		return 0;
	*at = (double)e.at / TMX1_HZ;
	*type = e.type;
	*arg = e.arg;
	return 1;
}

int
tmx1api_trace(tmx1 *m, int n, int *out)
{
	if (n > TMX1_TRACE)
		n = TMX1_TRACE;
	for (int i = 0; i < n; i++)
		out[i] = m->trace[(m->trace_pos + TMX1_TRACE - n + i) % TMX1_TRACE];
	return n;
}

void
tmx1api_audio_capture(tmx1 *m, int on)
{
	tmx1_audio_capture(m, on);
}

/* OUT2 (tone) edges: times in seconds and levels; returns count */
int
tmx1api_audio(tmx1 *m, int max, double *t, uint8_t *v)
{
	int n = (int)m->aud_n < max ? (int)m->aud_n : max;
	for (int i = 0; i < n; i++) {
		t[i] = (double)m->aud_t[i] / TMX1_HZ;
		v[i] = m->aud_v[i];
	}
	return n;
}

int tmx1api_audio_count(tmx1 *m) { return (int)m->aud_n; }
