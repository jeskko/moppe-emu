/*
 * HSN-2 / HSF-2 handset; see tmx1hs.h.
 */
#include <string.h>

#include "tmx1hs.h"

/* ------------------------------------------------------------ uPD7228 */

/* commands, as hsn2.asm (decrementing pointer) and hsf2.asm
 * (incrementing) use them: 0x64-0x6F write / OR / AND mode, 0x70-0x73
 * character mode, low 2 bits the pointer step (01 down, 10 up), 0x80+n
 * load data pointer, 0x08/0x09 display off/on, 0x10-0x1F frame and
 * multiplex settings (ignored) */
static void
lcd_command(upd7228 *l, uint8_t v)
{
	if (v & 0x80) {
		l->ptr = v & 0x7F;
		return;
	}
	if ((v & 0xF0) == 0x60 || (v & 0xFC) == 0x70) {
		static const int8_t step[4] = { 0, -1, 1, 0 };
		l->dir = step[v & 3];
		l->mode = (v & 0xFC) == 0x70 ? 3 : ((v >> 2) & 3) - 1;
		return;
	}
	if ((v & 0xFE) == 0x08)
		l->on = v & 1;
}

static void
lcd_put(upd7228 *l, uint8_t v)
{
	uint8_t *m = &l->ram[l->ptr & 0x7F];
	switch (l->mode) {
	case 0: *m = v; break;
	case 1: *m |= v; break;
	default: *m &= v; break;
	}
	if (l->mode == 0)
		l->chr[l->ptr & 0x7F] = 0;
	l->ptr = (uint8_t)((l->ptr + l->dir) & 0x7F);
}

static void
lcd_data(upd7228 *l, uint8_t v)
{
	l->writes++;
	if (l->mode != 3) {
		lcd_put(l, v);
		return;
	}
	/* character mode: 5 columns from the internal font.  No font ROM
	 * dump is in hand: the glyph bits (0..6) are left clear and the
	 * code is remembered at the first column; bit 7 (the icon row
	 * under the text) is kept, as the firmware's icons need */
	l->chr[l->ptr & 0x7F] = v ? v : ' ';
	for (int i = 0; i < 5; i++) {
		uint8_t *m = &l->ram[l->ptr & 0x7F];
		*m &= 0x80;
		if (i)
			l->chr[l->ptr & 0x7F] = 0;
		l->ptr = (uint8_t)((l->ptr + l->dir) & 0x7F);
	}
}

static void
lcd_pins(tmx1hs *h, int sck, int si, int cs, int cd)
{
	if (cs && !h->cs) {		/* CS released */
		h->sel = 0xFF;
	}
	if (!cs && h->cs) {		/* CS active: SCK counter cleared, chip address next */
		h->nbits = 0;
		h->first = 1;
		h->sel = 0xFF;
	}
	if (!cs && sck && !h->sck) {	/* SI clocked on the rising edge, MSB first */
		h->sr = (uint8_t)(h->sr << 1 | (si & 1));
		if (++h->nbits == 8) {
			h->nbits = 0;
			if (h->first) {
				h->first = 0;
				h->sel = h->sr == 0 ? 0 : h->sr == 3 ? 1 : 0xFF;
			} else if (h->sel != 0xFF) {
				if (cd)
					lcd_command(&h->lcd[h->sel], h->sr);
				else
					lcd_data(&h->lcd[h->sel], h->sr);
			}
		}
	}
	h->sck = (uint8_t)sck;
	h->si = (uint8_t)si;
	h->cs = (uint8_t)cs;
	h->cd = (uint8_t)cd;
}

/* ---------------------------------------------------------- PCD3312 */

static void
i2c_pins(tmx1hs *h, int scl, int sda)
{
	if (scl && h->scl && sda != h->sda) {
		if (!sda) {		/* START */
			h->i2c_on = 1;
			h->i2c_n = 0;
			h->i2c_byte = 0;
		} else			/* STOP */
			h->i2c_on = 0;
	} else if (h->i2c_on && scl && !h->scl) {
		if (h->i2c_n < 8)
			h->i2c_sr = (uint8_t)(h->i2c_sr << 1 | sda);
		if (++h->i2c_n == 9) {	/* 8 bits + acknowledge */
			h->i2c_n = 0;
			if (h->i2c_byte++ >= 1) {	/* after the address byte */
				h->dtmf = h->i2c_sr;
				h->dtmf_writes++;
				if (h->on_event)
					h->on_event(h->board, TMX1HS_EV_DTMF, h->i2c_sr);
			}
		}
	}
	h->scl = (uint8_t)scl;
	h->sda = (uint8_t)sda;
}

/* ------------------------------------------------------------ keypad */

/* key (row, col) closes rail 'row' onto column line AN4+col; a line is
 * high while a closed key's rail is high, and its falling edge sets the
 * AN flag ("trailing edge rises ANx if contact closed") */
static uint8_t
rails(tmx1hs *h)
{
	if (h->type == TMX1_HS_HSF2)
		return (uint8_t)(h->pa >> 1);	/* PA1..PA7 */
	return h->latch[0] & 0x7F;		/* latch D0, offsets 0..6 */
}

static void
kbd_update(tmx1hs *h)
{
	uint8_t r = rails(h), lv = 0;
	for (int s = 1; s <= 28; s++)
		if (h->pressed[s] && (r >> ((s - 1) / 4) & 1))
			lv |= (uint8_t)(1 << ((s - 1) % 4));
	for (int c = 0; c < 4; c++)
		if ((h->an_level >> c & 1) && !(lv >> c & 1))
			upd7810_an_edge(&h->cpu, c);
	h->an_level = lv;
}

void
tmx1hs_key(tmx1hs *h, int s, int down)
{
	if (s >= 1 && s <= 28)
		h->pressed[s] = (uint8_t)(down != 0);
	kbd_update(h);
}

void
tmx1hs_set_hook(tmx1hs *h, int offhook)
{
	h->offhook = offhook != 0;
}

void
tmx1hs_power_key(tmx1hs *h, int down)
{
	h->powersw = down != 0;
}

/* --------------------------------------------------------------- bus */

static uint8_t
hs_read(void *ctx, uint16_t a)
{
	tmx1hs *h = ctx;
	if (a >= 0xE000)
		return h->ram[a - 0xE000];
	return h->rom[a];
}

static void
hs_write(void *ctx, uint16_t a, uint8_t v)
{
	tmx1hs *h = ctx;
	if (a >= 0xE000) {
		h->ram[a - 0xE000] = v;
		return;
	}
	if (h->type == TMX1_HS_HSF2) {
		if (a >= 0x0400 && a < 0x0800)
			h->hsic[(a >> 8) & 3] = v;
		return;
	}
	/* HSN-2: two addressable latches over the EPROM, A2..A0 and D0 / D1 */
	{
		int n = a & 7;
		uint8_t m = (uint8_t)(1 << n);
		h->latch[0] = (uint8_t)((h->latch[0] & ~m) | ((v & 1) << n));
		h->latch[1] = (uint8_t)((h->latch[1] & ~m) | (((v >> 1) & 1) << n));
		kbd_update(h);
		lcd_pins(h, h->pa & 1, (h->pc >> 4) & 1, h->latch[1] & 1, (h->latch[1] >> 1) & 1);
	}
}

static uint8_t
hs_port_in(void *ctx, int p)
{
	tmx1hs *h = ctx;
	switch (p) {
	case UPD_PB:	/* PB0 POWERSW (0 = held), PB7 LCD /BUSY (never busy) */
		return (uint8_t)(0x80 | (h->powersw ? 0 : 1) | 0x7E);
	case UPD_PC:	/* PC3 OFFHOOK (1 = lifted) */
		return (uint8_t)(0xF7 | (h->offhook ? 0x08 : 0));
	}
	return 0xFF;
}

static void
hs_port_out(void *ctx, int p, uint8_t latch, uint8_t mode)
{
	tmx1hs *h = ctx;
	uint8_t pins = (uint8_t)(latch | mode);	/* inputs read as high */
	switch (p) {
	case UPD_PA: h->pa = pins; break;
	case UPD_PB: h->pb = pins; break;
	case UPD_PC: h->pc = pins; break;
	default: return;
	}
	if (h->type == TMX1_HS_HSF2) {
		/* SCK PA0, SI PC4, /CS PB3, C/D PC7 */
		lcd_pins(h, h->pa & 1, (h->pc >> 4) & 1, (h->pb >> 3) & 1, (h->pc >> 7) & 1);
		if (p == UPD_PA)
			kbd_update(h);
	} else {
		/* SCK PA0, SI PC4, /CS and C/D on latch D1 bits 0, 1 */
		lcd_pins(h, h->pa & 1, (h->pc >> 4) & 1, h->latch[1] & 1, (h->latch[1] >> 1) & 1);
		if (p == UPD_PC)
			i2c_pins(h, (h->pc >> 6) & 1, (h->pc >> 7) & 1);
	}
}

static void
hs_txd(void *ctx, int level)
{
	tmx1hs *h = ctx;
	if (h->on_txd)
		h->on_txd(h->board, level);
}

void
tmx1hs_reset(tmx1hs *h)
{
	h->latch[0] = h->latch[1] = 0;
	memset(h->hsic, 0, sizeof(h->hsic));
	h->sck = h->si = h->cs = 1;
	h->cd = 0;
	h->sel = 0xFF;
	h->scl = h->sda = 1;
	h->i2c_on = 0;
	h->dtmf = 0;
	h->an_level = 0;
	upd7810_reset(&h->cpu);
}

void
tmx1hs_init(tmx1hs *h, int type)
{
	upd7810_bus b = { 0 };
	void *board = h->board;
	void (*tx)(void *, int) = h->on_txd;
	void (*ev)(void *, int, int) = h->on_event;

	memset(h, 0, sizeof(*h));
	h->board = board;
	h->on_txd = tx;
	h->on_event = ev;
	h->type = type;
	memset(h->rom, 0xFF, sizeof(h->rom));
	b.ctx = h;
	b.read = hs_read;
	b.write = hs_write;
	b.port_in = hs_port_in;
	b.port_out = hs_port_out;
	b.txd = hs_txd;
	h->pa = h->pb = h->pc = 0xFF;
	upd7810_init(&h->cpu, &b);
	tmx1hs_reset(h);
}

/* ------------------------------------------------------------ display */

static char
cell(const upd7228 *l, int col, int dir)
{
	uint8_t c = l->chr[col & 0x7F];
	int any = 0;
	if (c)
		return (c >= 0x20 && c < 0x7F) ? (char)c : '?';
	for (int i = 0; i < 5; i++)
		any |= l->ram[(col + dir * i) & 0x7F] & 0x7F;
	return any ? '?' : ' ';
}

/* cell start columns: HSN-2 writes with a decrementing pointer from 49
 * (chip 3: middle row, then the two top cells) and from 39 (chip 0:
 * bottom row); HSF-2 increments from 0, its middle row in pieces */
static const int hsn_mid[8] = { 49, 44, 39, 34, 29, 24, 19, 14 };
static const int hsn_top[2] = { 9, 4 };
static const int hsn_bot[8] = { 39, 34, 29, 24, 19, 14, 9, 4 };
static const int hsf_mid[8] = { 0, 5, 10, 45, 15, 20, 25, 40 };
static const int hsf_top[2] = { 30, 35 };
static const int hsf_bot[8] = { 0, 5, 10, 15, 20, 25, 30, 35 };

void
tmx1hs_text(tmx1hs *h, char *mid, char *bot, char *top)
{
	int hsf = h->type == TMX1_HS_HSF2, d = hsf ? 1 : -1;
	for (int i = 0; i < 8; i++) {
		mid[i] = cell(&h->lcd[1], hsf ? hsf_mid[i] : hsn_mid[i], d);
		bot[i] = cell(&h->lcd[0], hsf ? hsf_bot[i] : hsn_bot[i], d);
	}
	for (int i = 0; i < 2; i++)
		top[i] = cell(&h->lcd[1], hsf ? hsf_top[i] : hsn_top[i], d);
	mid[8] = bot[8] = top[2] = 0;
}

int
tmx1hs_icons(tmx1hs *h)
{
	/* one column of each icon's bit-7 run (ICON_* in the sources) */
	static const int hsn[7] = { 46, 40, 38, 34, 27, 19, -1 };
	static const int hsf[7] = { 1, 4, 13, 31, 17, 21, 14 };
	const int *t = h->type == TMX1_HS_HSF2 ? hsf : hsn;
	int r = 0;
	for (int i = 0; i < 7; i++)
		if (t[i] >= 0 && (h->lcd[1].ram[t[i]] & 0x80))
			r |= 1 << i;
	return r;
}

int
tmx1hs_leds(tmx1hs *h)
{
	if (h->type == TMX1_HS_HSF2) {
		uint8_t v = h->hsic[3];
		return ((v >> 4) & 1) | ((v >> 1) & 1) << 1 | (v & 1) << 2
		    | ((v >> 2) & 1) << 3 | ((v >> 3) & 1) << 4
		    | ((v >> 5) & 1) << 5 | ((v >> 6) & 1) << 6;
	}
	/* latch D1: 2 ON, 3 SERV (lit at 0), 4 ROAM, 5 CALL, 6 HF, 7 MFT;
	 * latch D0 bit 7 BACKLIGHT */
	{
		uint8_t b = h->latch[1];
		return ((b >> 2) & 1) | (!((b >> 3) & 1)) << 1 | ((b >> 4) & 1) << 2
		    | ((b >> 5) & 1) << 3 | ((b >> 6) & 1) << 4 | ((b >> 7) & 1) << 5
		    | ((h->latch[0] >> 7) & 1) << 6;
	}
}
