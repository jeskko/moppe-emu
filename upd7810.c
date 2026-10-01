/*
 * NEC uPD7810 / uPD78C10 emulation; see upd7810.h.
 */
#include <string.h>

#include "upd7810.h"

/* ------------------------------------------------------------ memory */

static uint8_t
rd(upd7810 *c, uint16_t a)
{
	if (a >= 0xFF00 && (c->mm & 0x08))
		return c->iram[a & 0xFF];
	return c->bus.read(c->bus.ctx, a);
}

static void
wr(upd7810 *c, uint16_t a, uint8_t v)
{
	if (a >= 0xFF00 && (c->mm & 0x08)) {
		c->iram[a & 0xFF] = v;
		return;
	}
	c->bus.write(c->bus.ctx, a, v);
}

uint8_t
upd7810_peek(upd7810 *c, uint16_t a)
{
	return rd(c, a);
}

void
upd7810_poke(upd7810 *c, uint16_t a, uint8_t v)
{
	wr(c, a, v);
}

static uint8_t
fetch(upd7810 *c)
{
	return rd(c, c->pc++);
}

static uint16_t
fetch16(upd7810 *c)
{
	uint16_t lo = fetch(c);
	return (uint16_t)(lo | fetch(c) << 8);
}

static uint16_t
rd16(upd7810 *c, uint16_t a)
{
	uint16_t lo = rd(c, a);
	return (uint16_t)(lo | rd(c, (uint16_t)(a + 1)) << 8);
}

static void
wr16(upd7810 *c, uint16_t a, uint16_t v)
{
	wr(c, a, (uint8_t)v);
	wr(c, (uint16_t)(a + 1), (uint8_t)(v >> 8));
}

static void
push16(upd7810 *c, uint16_t v)
{
	wr(c, --c->sp, (uint8_t)(v >> 8));
	wr(c, --c->sp, (uint8_t)v);
}

static uint16_t
pop16(upd7810 *c)
{
	uint16_t lo = rd(c, c->sp++);
	return (uint16_t)(lo | rd(c, c->sp++) << 8);
}

/* ---------------------------------------------------------- registers */

#define BC ((uint16_t)(c->b << 8 | c->c))
#define DE ((uint16_t)(c->d << 8 | c->e))
#define HL ((uint16_t)(c->h << 8 | c->l))

static void set_bc(upd7810 *c, uint16_t v) { c->b = (uint8_t)(v >> 8); c->c = (uint8_t)v; }
static void set_de(upd7810 *c, uint16_t v) { c->d = (uint8_t)(v >> 8); c->e = (uint8_t)v; }
static void set_hl(upd7810 *c, uint16_t v) { c->h = (uint8_t)(v >> 8); c->l = (uint8_t)v; }

/* r: V A B C D E H L */
static uint8_t *
reg(upd7810 *c, int r)
{
	switch (r & 7) {
	case 0: return &c->v;
	case 1: return &c->a;
	case 2: return &c->b;
	case 3: return &c->c;
	case 4: return &c->d;
	case 5: return &c->e;
	case 6: return &c->h;
	default: return &c->l;
	}
}

/* r1: EAH EAL B C D E H L */
static uint8_t
get_r1(upd7810 *c, int r)
{
	if ((r & 7) == 0)
		return (uint8_t)(c->ea >> 8);
	if ((r & 7) == 1)
		return (uint8_t)c->ea;
	return *reg(c, r);
}

static void
set_r1(upd7810 *c, int r, uint8_t v)
{
	if ((r & 7) == 0)
		c->ea = (uint16_t)((c->ea & 0x00FF) | v << 8);
	else if ((r & 7) == 1)
		c->ea = (uint16_t)((c->ea & 0xFF00) | v);
	else
		*reg(c, r) = v;
}

/* rp: SP BC DE HL (EA for 4) */
static uint16_t
get_rp(upd7810 *c, int p)
{
	switch (p) {
	case 0: return c->sp;
	case 1: return BC;
	case 2: return DE;
	case 3: return HL;
	default: return c->ea;
	}
}

static void
set_rp(upd7810 *c, int p, uint16_t v)
{
	switch (p) {
	case 0: c->sp = v; break;
	case 1: set_bc(c, v); break;
	case 2: set_de(c, v); break;
	case 3: set_hl(c, v); break;
	default: c->ea = v; break;
	}
}

/* rp1 for PUSH/POP: VA BC DE HL EA */
static uint16_t
get_rp1(upd7810 *c, int p)
{
	return p == 0 ? (uint16_t)(c->v << 8 | c->a) : get_rp(c, p);
}

static void
set_rp1(upd7810 *c, int p, uint16_t v)
{
	if (p == 0) {
		c->v = (uint8_t)(v >> 8);
		c->a = (uint8_t)v;
	} else
		set_rp(c, p, v);
}

/* -------------------------------------------------------------- flags */

static void
flag(upd7810 *c, uint8_t f, int on)
{
	if (on)
		c->psw |= f;
	else
		c->psw &= (uint8_t)~f;
}

static void
skip_if(upd7810 *c, int cond)
{
	if (cond)
		c->psw |= UPD_SK;
}

static uint8_t
add8(upd7810 *c, uint8_t x, uint8_t y, int cin)
{
	unsigned r = (unsigned)x + y + cin;
	flag(c, UPD_CY, r > 0xFF);
	flag(c, UPD_HC, (x & 15) + (y & 15) + cin > 15);
	flag(c, UPD_Z, (r & 0xFF) == 0);
	return (uint8_t)r;
}

static uint8_t
sub8(upd7810 *c, uint8_t x, uint8_t y, int bin)
{
	int r = (int)x - y - bin;
	flag(c, UPD_CY, r < 0);
	flag(c, UPD_HC, (x & 15) - (y & 15) - bin < 0);
	flag(c, UPD_Z, (r & 0xFF) == 0);
	return (uint8_t)r;
}

/*
 * The ALU operation field shared by the register, memory, immediate,
 * working-register and special-register groups (bits 6..3 of the
 * operation byte).  Returns 1 when the result is to be stored.
 */
enum {
	OP_ANA = 1, OP_XRA, OP_ORA, OP_ADDNC, OP_GTA, OP_SUBNB, OP_LTA,
	OP_ADD, OP_ONA, OP_ADC, OP_OFFA, OP_SUB, OP_NEA, OP_SBB, OP_EQA
};

static int
alu8(upd7810 *c, int op, uint8_t x, uint8_t y, uint8_t *res)
{
	uint8_t r;
	switch (op) {
	case OP_ANA:
		r = x & y;
		flag(c, UPD_Z, r == 0);
		*res = r;
		return 1;
	case OP_XRA:
		r = x ^ y;
		flag(c, UPD_Z, r == 0);
		*res = r;
		return 1;
	case OP_ORA:
		r = x | y;
		flag(c, UPD_Z, r == 0);
		*res = r;
		return 1;
	case OP_ADDNC:
		*res = add8(c, x, y, 0);
		skip_if(c, !(c->psw & UPD_CY));
		return 1;
	case OP_GTA:
		sub8(c, x, y, 1);
		skip_if(c, !(c->psw & UPD_CY));
		return 0;
	case OP_SUBNB:
		*res = sub8(c, x, y, 0);
		skip_if(c, !(c->psw & UPD_CY));
		return 1;
	case OP_LTA:
		sub8(c, x, y, 0);
		skip_if(c, c->psw & UPD_CY);
		return 0;
	case OP_ADD:
		*res = add8(c, x, y, 0);
		return 1;
	case OP_ONA:
		flag(c, UPD_Z, (x & y) == 0);
		skip_if(c, (x & y) != 0);
		return 0;
	case OP_ADC:
		*res = add8(c, x, y, c->psw & UPD_CY);
		return 1;
	case OP_OFFA:
		flag(c, UPD_Z, (x & y) == 0);
		skip_if(c, (x & y) == 0);
		return 0;
	case OP_SUB:
		*res = sub8(c, x, y, 0);
		return 1;
	case OP_NEA:
		sub8(c, x, y, 0);
		skip_if(c, !(c->psw & UPD_Z));
		return 0;
	case OP_SBB:
		*res = sub8(c, x, y, c->psw & UPD_CY);
		return 1;
	case OP_EQA:
		sub8(c, x, y, 0);
		skip_if(c, c->psw & UPD_Z);
		return 0;
	}
	return 0;
}

/* 16-bit EA, rp3 group: same operation field */
static void
alu16(upd7810 *c, int op, uint16_t y)
{
	uint16_t x = c->ea;
	int32_t r;
	int cin, logic = 0;
	switch (op) {
	case OP_ANA: c->ea = x & y; logic = 1; break;
	case OP_XRA: c->ea = x ^ y; logic = 1; break;
	case OP_ORA: c->ea = x | y; logic = 1; break;
	case OP_ONA:
		flag(c, UPD_Z, (x & y) == 0);
		skip_if(c, (x & y) != 0);
		return;
	case OP_OFFA:
		flag(c, UPD_Z, (x & y) == 0);
		skip_if(c, (x & y) == 0);
		return;
	case OP_ADDNC: case OP_ADD: case OP_ADC:
		cin = op == OP_ADC ? (c->psw & UPD_CY) : 0;
		r = (int32_t)x + y + cin;
		flag(c, UPD_CY, r > 0xFFFF);
		flag(c, UPD_HC, (x & 15) + (y & 15) + cin > 15);
		flag(c, UPD_Z, (r & 0xFFFF) == 0);
		c->ea = (uint16_t)r;
		if (op == OP_ADDNC)
			skip_if(c, !(c->psw & UPD_CY));
		return;
	default:	/* GTA SUBNB LTA SUB NEA SBB EQA */
		cin = op == OP_GTA ? 1 : op == OP_SBB ? (c->psw & UPD_CY) : 0;
		r = (int32_t)x - y - cin;
		flag(c, UPD_CY, r < 0);
		flag(c, UPD_HC, (int)(x & 15) - (int)(y & 15) - cin < 0);
		flag(c, UPD_Z, (r & 0xFFFF) == 0);
		if (op == OP_SUB || op == OP_SUBNB || op == OP_SBB)
			c->ea = (uint16_t)r;
		if (op == OP_GTA || op == OP_SUBNB)
			skip_if(c, !(c->psw & UPD_CY));
		else if (op == OP_LTA)
			skip_if(c, c->psw & UPD_CY);
		else if (op == OP_NEA)
			skip_if(c, !(c->psw & UPD_Z));
		else if (op == OP_EQA)
			skip_if(c, c->psw & UPD_Z);
		return;
	}
	if (logic)
		flag(c, UPD_Z, c->ea == 0);
}

/* -------------------------------------------------------------- ports */

static void
txd_pin_update(upd7810 *c)
{
	/* PC0 is TxD in control mode, else the port bit (input = high) */
	int lvl;
	if (c->mcc & 0x01)
		lvl = c->txd;
	else if (c->mode[UPD_PC] & 0x01)
		lvl = 1;
	else
		lvl = c->port[UPD_PC] & 1;
	if (c->bus.txd)
		c->bus.txd(c->bus.ctx, lvl);
}

static void
port_changed(upd7810 *c, int p)
{
	if (c->bus.port_out)
		c->bus.port_out(c->bus.ctx, p, c->port[p], c->mode[p]);
	if (p == UPD_PC)
		txd_pin_update(c);
}

uint8_t
upd7810_port_read(upd7810 *c, int p)
{
	uint8_t pins = c->bus.port_in ? c->bus.port_in(c->bus.ctx, p) : 0xFF;
	uint8_t m = c->mode[p], v;
	if (p == UPD_PD)
		return c->port[p];
	v = (uint8_t)((c->port[p] & ~m) | (pins & m));
	if (p == UPD_PC && c->mcc) {
		uint8_t ctl = (uint8_t)((pins & ~0x23) | (c->txd & 1)
		    | (c->rxd & 1) << 1 | (c->ci & 1) << 5);
		v = (uint8_t)((v & ~c->mcc) | (ctl & c->mcc));
	}
	return v;
}

/* sr / sr1 / sr2 codes */
static uint8_t
sr_read(upd7810 *c, int s)
{
	switch (s) {
	case 0x00: return upd7810_port_read(c, UPD_PA);
	case 0x01: return upd7810_port_read(c, UPD_PB);
	case 0x02: return upd7810_port_read(c, UPD_PC);
	case 0x03: return upd7810_port_read(c, UPD_PD);
	case 0x05: return upd7810_port_read(c, UPD_PF);
	case 0x06: return c->mkh;
	case 0x07: return c->mkl;
	case 0x08: return c->anm;
	case 0x09: return c->smh;
	case 0x0A: return c->sml;
	case 0x0B: return c->eom;
	case 0x0C: return c->etmm;
	case 0x0D: return c->tmm;
	case 0x19: c->rx_full = 0; return c->rxb;
	case 0x20: case 0x21: case 0x22: case 0x23: return c->cr[s & 3];
	}
	return 0xFF;
}

static void
sr_write(upd7810 *c, int s, uint8_t v)
{
	switch (s) {
	case 0x00: c->port[UPD_PA] = v; port_changed(c, UPD_PA); break;
	case 0x01: c->port[UPD_PB] = v; port_changed(c, UPD_PB); break;
	case 0x02: c->port[UPD_PC] = v; port_changed(c, UPD_PC); break;
	case 0x03: c->port[UPD_PD] = v; break;
	case 0x05: c->port[UPD_PF] = v; port_changed(c, UPD_PF); break;
	case 0x06: c->mkh = v; break;
	case 0x07: c->mkl = v; break;
	case 0x08: c->anm = v; c->ad_idx = 0; c->ad_acc = 0; break;
	case 0x09: c->smh = v; break;
	case 0x0A: c->sml = v; break;
	case 0x0B: c->eom = v; break;
	case 0x0C: c->etmm = v; break;
	case 0x0D: c->tmm = v; break;
	case 0x10: c->mm = v; break;
	case 0x11: c->mcc = v; port_changed(c, UPD_PC); break;
	case 0x12: c->mode[UPD_PA] = v; port_changed(c, UPD_PA); break;
	case 0x13: c->mode[UPD_PB] = v; port_changed(c, UPD_PB); break;
	case 0x14: c->mode[UPD_PC] = v; port_changed(c, UPD_PC); break;
	case 0x17: c->mode[UPD_PF] = v; port_changed(c, UPD_PF); break;
	case 0x18: c->txb = v; c->txb_full = 1; break;
	case 0x1A: c->tm0 = v; break;
	case 0x1B: c->tm1 = v; break;
	case 0x28: c->zcm = v; break;
	}
}

/* -------------------------------------------------------- peripherals */

static void
set_txd(upd7810 *c, int lvl)
{
	if (c->txd != lvl) {
		c->txd = (uint8_t)lvl;
		if (c->mcc & 0x01)
			txd_pin_update(c);
	}
}

/* SML: B (1..0) clock factor, L (3..2) length, PEN 4, EP 5, S (7..6) stop */
static int ser_ticks_per_bit(const upd7810 *c)
{
	static const int f[4] = { 16, 1, 16, 64 };
	return f[c->sml & 3];
}

static int ser_data_bits(const upd7810 *c) { return 5 + ((c->sml >> 2) & 3); }

static void
ser_clock(upd7810 *c)
{
	int tpb = ser_ticks_per_bit(c), nd = ser_data_bits(c);
	int pen = (c->sml >> 4) & 1;

	/* transmitter */
	if (!c->tx_busy && c->txb_full && (c->smh & 0x08)) {
		uint16_t f = (uint16_t)((c->txb & ((1 << nd) - 1)) << 1);
		int n = 1 + nd, ones = __builtin_popcount(c->txb & ((1 << nd) - 1));
		if (pen) {
			int par = (c->sml & 0x20) ? (ones & 1) : !(ones & 1);
			f |= (uint16_t)(par << n);
			n++;
		}
		f |= (uint16_t)(((c->sml >> 6) == 3 ? 3 : 1) << n);
		n += (c->sml >> 6) == 3 ? 2 : 1;
		c->tx_shift = f;
		c->tx_nbits = (uint8_t)n;
		c->tx_busy = 1;
		c->tx_tick = 0;
		c->txb_full = 0;
		c->irr |= 1u << UPD_FST;
	}
	if (c->tx_busy) {
		if (c->tx_tick == 0) {
			set_txd(c, c->tx_shift & 1);
			c->tx_shift >>= 1;
			c->tx_nbits--;
		}
		if (++c->tx_tick >= tpb) {
			c->tx_tick = 0;
			if (c->tx_nbits == 0) {
				c->tx_busy = 0;
				set_txd(c, 1);
			}
		}
	}

	/* receiver */
	if (!(c->smh & 0x04)) {
		c->rx_state = 0;
		return;
	}
	switch (c->rx_state) {
	case 0:
		if (!c->rxd) {
			c->rx_state = 1;
			c->rx_tick = 0;
		}
		break;
	case 1:		/* start bit: confirm at its middle */
		if (++c->rx_tick >= tpb / 2) {
			if (c->rxd)
				c->rx_state = 0;
			else {
				c->rx_state = 2;
				c->rx_tick = 0;
				c->rx_nbits = 0;
				c->rx_shift = 0;
			}
		}
		break;
	case 2:
		if (++c->rx_tick >= tpb) {
			int total = nd + pen + 1;
			c->rx_tick = 0;
			c->rx_shift |= (uint16_t)((c->rxd & 1) << c->rx_nbits);
			if (++c->rx_nbits == total) {
				uint8_t d = (uint8_t)(c->rx_shift & ((1 << nd) - 1));
				int err = !((c->rx_shift >> (nd + pen)) & 1);
				if (pen) {
					int ones = __builtin_popcount(c->rx_shift & ((1 << (nd + 1)) - 1));
					err |= (c->sml & 0x20) ? (ones & 1) : !(ones & 1);
				}
				if (c->rx_full)
					err = 1;	/* overrun */
				if (err)
					c->irr |= 1u << UPD_ER;
				c->rxb = d;
				c->rx_full = 1;
				c->irr |= 1u << UPD_FSR;
				c->rx_state = 0;
			}
		}
		break;
	}
}

/* timer flip-flop toggled; serial clock from TO (SMH bits 1..0 = 00) on its rising edge */
static void
tff_toggle(upd7810 *c)
{
	c->tff ^= 1;
	if (c->tff && (c->smh & 3) == 0)
		ser_clock(c);
}

static unsigned
tmr_adv(uint8_t *cnt, uint8_t tm, unsigned n)
{
	unsigned per = tm ? tm : 256, tot;
	if (*cnt >= per)
		*cnt = 0;
	tot = *cnt + n;
	*cnt = (uint8_t)(tot % per);
	return tot / per;
}

static void
ecnt_inc(upd7810 *c, unsigned n)
{
	while (n--) {
		c->ecnt++;
		if (c->ecnt == c->etm0)
			c->irr |= 1u << UPD_FE0;
		if (c->ecnt == c->etm1) {
			c->irr |= 1u << UPD_FE1;
			if (((c->etmm >> 2) & 3) == 3)
				c->ecnt = 0;
		}
	}
}

static void
tick(upd7810 *c, int n)
{
	uint64_t s0 = c->states, s1 = s0 + (unsigned)n;
	unsigned p12 = (unsigned)(s1 / 4 - s0 / 4);	/* f/12: every 4 states */
	unsigned p384 = (unsigned)(s1 / 128 - s0 / 128);
	unsigned in0, in1, m0, m1, i;
	int ffsrc = c->tmm & 3;

	c->states = s1;

	/* TMM: T0 clock bits 3..2, T1 clock bits 6..5 (00 f/12, 01 f/384,
	 * 10 T0 / TI, 11 stop), F/F input bits 1..0 (00 T0, 01 T1) */
	switch ((c->tmm >> 2) & 3) {
	case 0: in0 = p12; break;
	case 1: in0 = p384; break;
	default: in0 = 0; break;
	}
	m0 = in0 ? tmr_adv(&c->cnt0, c->tm0, in0) : 0;
	switch ((c->tmm >> 5) & 3) {
	case 0: in1 = p12; break;
	case 1: in1 = p384; break;
	case 2: in1 = m0; break;
	default: in1 = 0; break;
	}
	m1 = in1 ? tmr_adv(&c->cnt1, c->tm1, in1) : 0;
	if (m0)
		c->irr |= 1u << UPD_FT0;
	if (m1)
		c->irr |= 1u << UPD_FT1;
	if (ffsrc == 0)
		for (i = 0; i < m0; i++)
			tff_toggle(c);
	else if (ffsrc == 1)
		for (i = 0; i < m1; i++)
			tff_toggle(c);

	/* ETMM bits 1..0: 00 f/12 (others: CI events, counted in set_ci) */
	if ((c->etmm & 3) == 0 && p12)
		ecnt_inc(c, p12);

	/* A/D: scan (MS = 0) AN0-3 / AN4-7 by ANI2, or select AN(ANI); 4 results then INTFAD */
	{
		unsigned conv = (c->anm & 0x10) ? 144 : 192;
		c->ad_acc += (unsigned)n;
		while (c->ad_acc >= conv) {
			int ch = (c->anm & 1) ? (c->anm >> 1) & 7
			    : (c->anm & 0x08 ? 4 : 0) + c->ad_idx;
			c->ad_acc -= conv;
			c->cr[c->ad_idx] = c->bus.adc ? c->bus.adc(c->bus.ctx, ch) : 0;
			if (++c->ad_idx == 4) {
				c->ad_idx = 0;
				c->irr |= 1u << UPD_FAD;
			}
		}
	}
}

void
upd7810_set_rxd(upd7810 *c, int level)
{
	c->rxd = (uint8_t)(level != 0);
}

void
upd7810_set_ci(upd7810 *c, int level)
{
	level = level != 0;
	if (c->ci && !level && (c->mcc & 0x20)) {
		c->irr |= 1u << UPD_FEIN;
		if (((c->etmm >> 2) & 3) == 2)
			c->ecnt = 0;
		if ((c->etmm & 3) == 2)
			ecnt_inc(c, 1);
	}
	c->ci = (uint8_t)level;
}

void
upd7810_an_edge(upd7810 *c, int n)
{
	c->irr |= 1u << (UPD_AN4 + (n & 3));
}

void
upd7810_nmi(upd7810 *c)
{
	c->irr |= 1u << UPD_FNMI;
}

/* --------------------------------------------------------- interrupts */

static int
enter_irq(upd7810 *c, uint16_t vec)
{
	wr(c, --c->sp, c->psw);
	push16(c, c->pc);
	c->psw &= (uint8_t)~(UPD_SK | UPD_L0 | UPD_L1);
	c->ie = 0;
	c->pc = vec;
	return 13;
}

static uint32_t
unmasked(const upd7810 *c)
{
	uint32_t mask = (uint32_t)c->mkl | (uint32_t)c->mkh << 8;
	return c->irr & ~mask & 0x7FE;
}

static int
irq(upd7810 *c)
{
	static const uint16_t vec[11] = { 0x04, 0x08, 0x08, 0x10, 0x10,
	    0x18, 0x18, 0x20, 0x20, 0x28, 0x28 };
	uint32_t pend;
	int i;

	if (c->irr & (1u << UPD_FNMI)) {
		c->irr &= ~(1u << UPD_FNMI);
		return enter_irq(c, 0x04);
	}
	if (!c->ie || !(pend = unmasked(c)))
		return 0;
	for (i = 1; i <= 10; i++) {
		if (pend & (1u << i)) {
			/* two sources share each vector: the flag is cleared
			 * on entry only when the other one is masked */
			int other = (i & 1) ? i + 1 : i - 1;
			uint32_t mask = (uint32_t)c->mkl | (uint32_t)c->mkh << 8;
			if (mask & (1u << other))
				c->irr &= ~(1u << i);
			return enter_irq(c, vec[i]);
		}
	}
	return 0;
}

/* ------------------------------------------------------------ decoder */

/* first-byte lengths; 0 = prefix (see oplen), -1 undefined (1 byte) */
static const int8_t len1[256] = {
/*       0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
/* 0 */  1, 2, 1, 1, 3, 3,-1, 2, 1, 1, 1, 1, 1, 1, 1, 1,
/* 1 */  1, 1, 1, 1, 3, 3, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1,
/* 2 */  2, 1, 1, 1, 3, 3, 2, 2,-1, 1, 1, 1, 1, 1, 1, 1,
/* 3 */  2, 1, 1, 1, 3, 3, 2, 2,-1, 1, 1, 1, 1, 1, 1, 1,
/* 4 */  3, 1, 1, 1, 3, 3, 2, 2, 0, 2, 2, 2, 2, 2, 2, 2,
/* 5 */  1, 1, 1, 1, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
/* 6 */  2, 1, 1, 2, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
/* 7 */  0, 3, 1,-1, 0, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
/* 8 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* 9 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* A */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 2,
/* B */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 2,
/* C */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* D */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* E */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* F */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};

static int
oplen2(uint8_t op, uint8_t b2)
{
	int l = len1[op];
	if (l > 0)
		return l;
	if (l < 0)
		return 1;
	switch (op) {
	case 0x48:	/* LDEAX / STEAX [DE+byte] [HL+byte] */
		if ((b2 & 0xE0) == 0x80 && ((b2 & 0x0F) == 0x0B || (b2 & 0x0F) == 0x0F))
			return 3;
		return 2;
	case 0x70:	/* SSPD..LHLD word, MOV r, word, MOV word, r */
		if ((b2 & 0xCE) == 0x0E || (b2 & 0xE8) == 0x68)
			return 4;
		return 2;
	case 0x74:
		if (b2 & 0x80)
			return (b2 & 0x07) == 0 ? 3 : 2;
		return 3;
	}
	return 1;
}

int
upd7810_oplen(upd7810 *c, uint16_t a)
{
	return oplen2(rd(c, a), rd(c, (uint16_t)(a + 1)));
}

/* states of a skipped instruction (data sheet remark) */
static int
skip_states(uint8_t op, int len)
{
	int starred = len1[op] > 0 || op == 0x4C || op == 0x4D;
	switch (len) {
	case 1: return 4;
	case 2: return starred ? 7 : 8;
	case 3: return starred ? 10 : 11;
	default: return 14;
	}
}

/* rpa2 addressing (also rpa, rpa1, and rpa3 with step 2 for ++) */
static uint16_t
addr_rpa(upd7810 *c, int m, int step)
{
	uint16_t t;
	switch (m) {
	case 0x1: return BC;
	case 0x2: return DE;
	case 0x3: return HL;
	case 0x4: t = DE; set_de(c, (uint16_t)(t + step)); return t;
	case 0x5: t = HL; set_hl(c, (uint16_t)(t + step)); return t;
	case 0x6: t = DE; set_de(c, (uint16_t)(t - 1)); return t;
	case 0x7: t = HL; set_hl(c, (uint16_t)(t - 1)); return t;
	case 0xB: return (uint16_t)(DE + fetch(c));
	case 0xC: return (uint16_t)(HL + c->a);
	case 0xD: return (uint16_t)(HL + c->b);
	case 0xE: return (uint16_t)(HL + c->ea);
	case 0xF: return (uint16_t)(HL + fetch(c));
	}
	c->illegal = 1;
	return 0;
}

static int
rpa_long(int m)
{
	return m >= 0xB;
}

static int
test_irf(upd7810 *c, int f)
{
	int set = (c->irr >> f) & 1;
	c->irr &= ~(1u << f);
	return set;
}

static int
exec48(upd7810 *c)
{
	uint8_t op = fetch(c), t;
	int r = op & 3;
	uint16_t a;
	unsigned w;

	switch (op) {
	case 0x01: case 0x02: case 0x03:	/* SLRC r2 */
		t = *reg(c, r);
		flag(c, UPD_CY, t & 1);
		*reg(c, r) = t >> 1;
		skip_if(c, c->psw & UPD_CY);
		return 8;
	case 0x05: case 0x06: case 0x07:	/* SLLC r2 */
		t = *reg(c, r);
		flag(c, UPD_CY, t & 0x80);
		*reg(c, r) = (uint8_t)(t << 1);
		skip_if(c, c->psw & UPD_CY);
		return 8;
	case 0x0A: skip_if(c, c->psw & UPD_CY); return 8;	/* SK */
	case 0x0B: skip_if(c, c->psw & UPD_HC); return 8;
	case 0x0C: skip_if(c, c->psw & UPD_Z); return 8;
	case 0x1A: skip_if(c, !(c->psw & UPD_CY)); return 8;	/* SKN */
	case 0x1B: skip_if(c, !(c->psw & UPD_HC)); return 8;
	case 0x1C: skip_if(c, !(c->psw & UPD_Z)); return 8;
	case 0x21: case 0x22: case 0x23:	/* SLR */
		t = *reg(c, r);
		flag(c, UPD_CY, t & 1);
		*reg(c, r) = t >> 1;
		return 8;
	case 0x25: case 0x26: case 0x27:	/* SLL */
		t = *reg(c, r);
		flag(c, UPD_CY, t & 0x80);
		*reg(c, r) = (uint8_t)(t << 1);
		return 8;
	case 0x28: c->pc = c->ea; return 8;	/* JEA */
	case 0x29:				/* CALB */
		push16(c, c->pc);
		c->pc = BC;
		return 17;
	case 0x2A: c->psw &= (uint8_t)~UPD_CY; return 8;	/* CLC */
	case 0x2B: c->psw |= UPD_CY; return 8;		/* STC */
	case 0x2D: case 0x2E: case 0x2F:	/* MUL r2 */
		c->ea = (uint16_t)(c->a * *reg(c, r));
		return 32;
	case 0x31: case 0x32: case 0x33:	/* RLR */
		t = *reg(c, r);
		*reg(c, r) = (uint8_t)(t >> 1 | (c->psw & UPD_CY) << 7);
		flag(c, UPD_CY, t & 1);
		return 8;
	case 0x35: case 0x36: case 0x37:	/* RLL */
		t = *reg(c, r);
		*reg(c, r) = (uint8_t)(t << 1 | (c->psw & UPD_CY));
		flag(c, UPD_CY, t & 0x80);
		return 8;
	case 0x38:				/* RLD */
		a = HL;
		t = rd(c, a);
		wr(c, a, (uint8_t)(t << 4 | (c->a & 15)));
		c->a = (uint8_t)((c->a & 0xF0) | t >> 4);
		return 17;
	case 0x39:				/* RRD */
		a = HL;
		t = rd(c, a);
		wr(c, a, (uint8_t)((c->a & 15) << 4 | t >> 4));
		c->a = (uint8_t)((c->a & 0xF0) | (t & 15));
		return 17;
	case 0x3A:				/* NEGA */
		c->a = sub8(c, 0, c->a, 0);
		return 8;
	case 0x3B:				/* HLT */
		c->halt = 1;
		return 12;
	case 0x3D: case 0x3E: case 0x3F:	/* DIV r2 */
		t = *reg(c, r);
		if (t) {
			w = c->ea;
			c->ea = (uint16_t)(w / t);
			*reg(c, r) = (uint8_t)(w % t);
		} else {
			*reg(c, r) = (uint8_t)c->ea;
			c->ea = 0xFFFF;
		}
		return 59;
	case 0xA0:				/* DSLR */
		flag(c, UPD_CY, c->ea & 1);
		c->ea >>= 1;
		return 8;
	case 0xA4:				/* DSLL */
		flag(c, UPD_CY, c->ea & 0x8000);
		c->ea = (uint16_t)(c->ea << 1);
		return 8;
	case 0xA8:				/* TABLE */
		a = (uint16_t)(c->pc + 1 + c->a);
		c->c = rd(c, a);
		c->b = rd(c, (uint16_t)(a + 1));
		return 17;
	case 0xB0:				/* DRLR */
		w = c->ea;
		c->ea = (uint16_t)(w >> 1 | (c->psw & UPD_CY) << 15);
		flag(c, UPD_CY, w & 1);
		return 8;
	case 0xB4:				/* DRLL */
		w = c->ea;
		c->ea = (uint16_t)(w << 1 | (c->psw & UPD_CY));
		flag(c, UPD_CY, w & 0x8000);
		return 8;
	case 0xBB:				/* STOP */
		c->stopped = 1;
		return 12;
	case 0xC0: c->ea = c->ecnt; return 14;	/* DMOV EA, ECNT */
	case 0xC1: c->ea = c->ecpt; return 14;
	case 0xD2: c->etm0 = c->ea; return 14;	/* DMOV ETM0, EA */
	case 0xD3: c->etm1 = c->ea; return 14;
	}
	if (op >= 0x40 && op < 0x80) {		/* SKIT / SKNIT irf */
		int f = op & 0x1F, set;
		if (f > UPD_SB || (f > UPD_OV && f < UPD_AN4)) {
			c->illegal = 1;
			return 8;
		}
		set = test_irf(c, f);
		skip_if(c, op < 0x60 ? set : !set);
		return 8;
	}
	if ((op & 0xE0) == 0x80) {		/* LDEAX / STEAX rpa3 */
		int m = op & 0x0F;
		if (m < 2 || m == 6 || m == 7 || (m > 7 && m < 0xB)) {
			c->illegal = 1;
			return 8;
		}
		a = addr_rpa(c, m, 2);
		if (op & 0x10)
			wr16(c, a, c->ea);
		else
			c->ea = rd16(c, a);
		return rpa_long(m) ? 20 : 14;
	}
	c->illegal = 1;
	return 8;
}

static int
exec60(upd7810 *c)
{
	uint8_t op = fetch(c), res;
	int alu = (op >> 3) & 15, r = op & 7;
	uint8_t *rp = reg(c, r);
	if (alu == 0 || (!(op & 0x80) && (alu == OP_ONA || alu == OP_OFFA))) {
		c->illegal = 1;
		return 8;
	}
	if (op & 0x80) {
		if (alu8(c, alu, c->a, *rp, &res))
			c->a = res;
	} else {
		if (alu8(c, alu, *rp, c->a, &res))
			*rp = res;
	}
	return 8;
}

static int
exec64(upd7810 *c)
{
	uint8_t op = fetch(c), d = fetch(c), res;
	int s = (op & 7) | (op >> 4 & 8), alu = (op >> 3) & 15;
	/* sr2: PA PB PC PD PF MKH MKL ANM SMH EOM TMM */
	if (s == 4 || s == 10 || s == 12 || s > 13) {
		c->illegal = 1;
		return 14;
	}
	if (alu == 0) {			/* MVI sr2, byte */
		sr_write(c, s, d);
		return 14;
	}
	if (alu8(c, alu, sr_read(c, s), d, &res)) {
		sr_write(c, s, res);
		return 20;
	}
	return 14;
}

static int
exec70(upd7810 *c)
{
	uint8_t op = fetch(c), res;
	uint16_t a;
	int alu = (op >> 3) & 15;

	switch (op) {
	case 0x0E: wr16(c, fetch16(c), c->sp); return 20;	/* SSPD */
	case 0x0F: c->sp = rd16(c, fetch16(c)); return 20;	/* LSPD */
	case 0x1E: wr16(c, fetch16(c), BC); return 20;	/* SBCD */
	case 0x1F: set_bc(c, rd16(c, fetch16(c))); return 20;
	case 0x2E: wr16(c, fetch16(c), DE); return 20;	/* SDED */
	case 0x2F: set_de(c, rd16(c, fetch16(c))); return 20;
	case 0x3E: wr16(c, fetch16(c), HL); return 20;	/* SHLD */
	case 0x3F: set_hl(c, rd16(c, fetch16(c))); return 20;
	case 0x41: case 0x42: case 0x43:		/* EADD EA, r2 */
		alu16(c, OP_ADD, *reg(c, op & 3));
		return 11;
	case 0x61: case 0x62: case 0x63:		/* ESUB EA, r2 */
		alu16(c, OP_SUB, *reg(c, op & 3));
		return 11;
	}
	if ((op & 0xF8) == 0x68) {			/* MOV r, word */
		*reg(c, op & 7) = rd(c, fetch16(c));
		return 17;
	}
	if ((op & 0xF8) == 0x78) {			/* MOV word, r */
		wr(c, fetch16(c), *reg(c, op & 7));
		return 17;
	}
	if ((op & 0x80) && (op & 7) && alu) {		/* xxxX rpa */
		a = addr_rpa(c, op & 7, 1);
		if (alu8(c, alu, c->a, rd(c, a), &res))
			c->a = res;
		return 11;
	}
	c->illegal = 1;
	return 8;
}

static int
exec74(upd7810 *c)
{
	uint8_t op = fetch(c), d, res;
	int alu = (op >> 3) & 15;

	if (!alu) {
		c->illegal = 1;
		return 8;
	}
	if (!(op & 0x80)) {			/* xxI r, byte */
		uint8_t *rp = reg(c, op & 7);
		d = fetch(c);
		if (alu8(c, alu, *rp, d, &res))
			*rp = res;
		return 11;
	}
	if ((op & 7) == 0) {			/* xxxW wa */
		d = fetch(c);
		if (alu8(c, alu, c->a, rd(c, (uint16_t)(c->v << 8 | d)), &res))
			c->a = res;
		return 14;
	}
	if ((op & 7) >= 5) {			/* Dxxx EA, rp3 */
		alu16(c, alu, get_rp(c, op & 3));
		return 11;
	}
	c->illegal = 1;
	return 8;
}

/* 1-byte A, byte immediate group: ((op >> 1) << 4) | 6 | (op & 1) */
static int
imm_a_op(uint8_t op)
{
	return ((op >> 4) << 1) | (op & 1);
}

static int
exec(upd7810 *c)
{
	uint8_t op, d, res, t;
	uint16_t a, w;
	int n;

	c->op_pc = c->pc;
	op = fetch(c);

	switch (op) {
	case 0x00: return 4;					/* NOP */
	case 0x01:						/* LDAW wa */
		c->a = rd(c, (uint16_t)(c->v << 8 | fetch(c)));
		return 10;
	case 0x02: case 0x12: case 0x22: case 0x32:		/* INX rp */
		set_rp(c, op >> 4, (uint16_t)(get_rp(c, op >> 4) + 1));
		return 7;
	case 0x03: case 0x13: case 0x23: case 0x33:		/* DCX rp */
		set_rp(c, op >> 4, (uint16_t)(get_rp(c, op >> 4) - 1));
		return 7;
	case 0x04: case 0x14: case 0x24: case 0x34: case 0x44:	/* LXI rp2 */
		set_rp(c, op >> 4, fetch16(c));
		return 10;
	case 0x05: case 0x15: case 0x25: case 0x35:		/* xxIW wa, byte */
	case 0x45: case 0x55: case 0x65: case 0x75:
		a = (uint16_t)(c->v << 8 | fetch(c));
		d = fetch(c);
		if (alu8(c, imm_a_op(op), rd(c, a), d, &res)) {
			wr(c, a, res);
			return 19;
		}
		return 13;
	case 0x07: case 0x16: case 0x17: case 0x26: case 0x27:	/* xxI A, byte */
	case 0x36: case 0x37: case 0x46: case 0x47: case 0x56:
	case 0x57: case 0x66: case 0x67: case 0x76: case 0x77:
		d = fetch(c);
		if (alu8(c, imm_a_op(op), c->a, d, &res))
			c->a = res;
		return 7;
	case 0x08: case 0x09: case 0x0A: case 0x0B:		/* MOV A, r1 */
	case 0x0C: case 0x0D: case 0x0E: case 0x0F:
		c->a = get_r1(c, op);
		return 4;
	case 0x10:						/* EXA */
		t = c->v; c->v = c->v2; c->v2 = t;
		t = c->a; c->a = c->a2; c->a2 = t;
		w = c->ea; c->ea = c->ea2; c->ea2 = w;
		return 4;
	case 0x11:						/* EXX */
		t = c->b; c->b = c->b2; c->b2 = t;
		t = c->c; c->c = c->c2; c->c2 = t;
		t = c->d; c->d = c->d2; c->d2 = t;
		t = c->e; c->e = c->e2; c->e2 = t;
		/* fall through */
	case 0x50:						/* EXH */
		t = c->h; c->h = c->h2; c->h2 = t;
		t = c->l; c->l = c->l2; c->l2 = t;
		return 4;
	case 0x18: case 0x19: case 0x1A: case 0x1B:		/* MOV r1, A */
	case 0x1C: case 0x1D: case 0x1E: case 0x1F:
		set_r1(c, op, c->a);
		return 4;
	case 0x20:						/* INRW wa */
		a = (uint16_t)(c->v << 8 | fetch(c));
		t = rd(c, a);
		res = add8(c, t, 1, 0);
		wr(c, a, res);
		skip_if(c, c->psw & UPD_CY);
		return 16;
	case 0x30:						/* DCRW wa */
		a = (uint16_t)(c->v << 8 | fetch(c));
		t = rd(c, a);
		res = sub8(c, t, 1, 0);
		wr(c, a, res);
		skip_if(c, c->psw & UPD_CY);
		return 16;
	case 0x21: c->pc = BC; return 4;			/* JB */
	case 0x29: case 0x2A: case 0x2B: case 0x2C:		/* LDAX rpa2 */
	case 0x2D: case 0x2E: case 0x2F:
	case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
		n = (op & 7) | (op >> 4 & 8);
		c->a = rd(c, addr_rpa(c, n, 1));
		return rpa_long(n) ? 13 : 7;
	case 0x39: case 0x3A: case 0x3B: case 0x3C:		/* STAX rpa2 */
	case 0x3D: case 0x3E: case 0x3F:
	case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
		n = (op & 7) | (op >> 4 & 8);
		wr(c, addr_rpa(c, n, 1), c->a);
		return rpa_long(n) ? 13 : 7;
	case 0x31:						/* BLOCK */
		n = 0;
		do {
			wr(c, DE, rd(c, HL));
			set_de(c, (uint16_t)(DE + 1));
			set_hl(c, (uint16_t)(HL + 1));
			n += 13;
		} while (c->c-- != 0);
		return n;
	case 0x40:						/* CALL word */
		a = fetch16(c);
		push16(c, c->pc);
		c->pc = a;
		return 16;
	case 0x41: case 0x42: case 0x43:			/* INR r2 */
		t = *reg(c, op & 3);
		*reg(c, op & 3) = add8(c, t, 1, 0);
		skip_if(c, c->psw & UPD_CY);
		return 4;
	case 0x51: case 0x52: case 0x53:			/* DCR r2 */
		t = *reg(c, op & 3);
		*reg(c, op & 3) = sub8(c, t, 1, 0);
		skip_if(c, c->psw & UPD_CY);
		return 4;
	case 0x48: return exec48(c);
	case 0x49: case 0x4A: case 0x4B:			/* MVIX rpa1, byte */
		wr(c, addr_rpa(c, op & 3, 1), fetch(c));
		return 10;
	case 0x4C:						/* MOV A, sr1 */
		d = fetch(c);
		if ((d & 0xC0) != 0xC0) {
			c->illegal = 1;
			return 10;
		}
		c->a = sr_read(c, d & 0x3F);
		return 10;
	case 0x4D:						/* MOV sr, A */
		d = fetch(c);
		if ((d & 0xC0) != 0xC0) {
			c->illegal = 1;
			return 10;
		}
		sr_write(c, d & 0x3F, c->a);
		return 10;
	case 0x4E: case 0x4F:					/* JRE */
		d = fetch(c);
		w = (uint16_t)((op & 1) << 8 | d);
		c->pc = (uint16_t)(c->pc + ((w & 0x100) ? (int)w - 0x200 : (int)w));
		return 10;
	case 0x54:						/* JMP word */
		c->pc = fetch16(c);
		return 10;
	case 0x58: case 0x59: case 0x5A: case 0x5B:		/* BIT bit, wa */
	case 0x5C: case 0x5D: case 0x5E: case 0x5F:
		t = rd(c, (uint16_t)(c->v << 8 | fetch(c)));
		skip_if(c, (t >> (op & 7)) & 1);
		return 10;
	case 0x60: return exec60(c);
	case 0x61:						/* DAA */
		t = c->a;
		{
			unsigned adj = 0, cy = c->psw & UPD_CY;
			if ((c->psw & UPD_HC) || (t & 15) > 9)
				adj |= 0x06;
			if (cy || t > 0x99 || ((t & 15) > 9 && t >= 0x90))
				adj |= 0x60, cy = 1;
			c->a = add8(c, t, (uint8_t)adj, 0);
			flag(c, UPD_CY, cy);
		}
		return 4;
	case 0x62:						/* RETI */
		c->pc = pop16(c);
		c->psw = rd(c, c->sp++);
		return 13;
	case 0x63:						/* STAW wa */
		wr(c, (uint16_t)(c->v << 8 | fetch(c)), c->a);
		return 10;
	case 0x64: return exec64(c);
	case 0x68: case 0x6A: case 0x6B:			/* MVI r, byte */
	case 0x6C: case 0x6D: case 0x6E: case 0x6F:
		*reg(c, op & 7) = fetch(c);
		return 7;
	case 0x69:						/* MVI A, byte (L1 set in exec_one) */
		c->a = fetch(c);
		return 7;
	case 0x70: return exec70(c);
	case 0x71:						/* MVIW wa, byte */
		a = (uint16_t)(c->v << 8 | fetch(c));
		wr(c, a, fetch(c));
		return 13;
	case 0x72:						/* SOFTI */
		wr(c, --c->sp, c->psw);
		push16(c, c->pc);
		c->pc = 0x0060;
		return 16;
	case 0x74: return exec74(c);
	case 0x78: case 0x79: case 0x7A: case 0x7B:		/* CALF */
	case 0x7C: case 0x7D: case 0x7E: case 0x7F:
		a = (uint16_t)(0x0800 | (op & 7) << 8 | fetch(c));
		push16(c, c->pc);
		c->pc = a;
		return 13;
	case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4:	/* POP rp1 */
		set_rp1(c, op & 7, pop16(c));
		return 10;
	case 0xA5: case 0xA6: case 0xA7:			/* DMOV EA, rp3 */
		c->ea = get_rp(c, op & 3);
		return 4;
	case 0xA8: c->ea++; return 7;				/* INX EA */
	case 0xA9: c->ea--; return 7;				/* DCX EA */
	case 0xAA: c->ie = 1; c->ei_delay = 1; return 4;	/* EI */
	case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4:	/* PUSH rp1 */
		push16(c, get_rp1(c, op & 7));
		return 13;
	case 0xB5: case 0xB6: case 0xB7:			/* DMOV rp3, EA */
		set_rp(c, op & 3, c->ea);
		return 4;
	case 0xB8: c->pc = pop16(c); return 10;			/* RET */
	case 0xB9: c->pc = pop16(c); c->psw |= UPD_SK; return 10;	/* RETS */
	case 0xBA: c->ie = 0; return 4;				/* DI */
	}
	if (op >= 0x80 && op < 0xA0) {				/* CALT */
		a = rd16(c, (uint16_t)(0x80 + 2 * (op & 0x1F)));
		push16(c, c->pc);
		c->pc = a;
		return 16;
	}
	if (op >= 0xC0) {					/* JR */
		int disp = op & 0x3F;
		if (disp & 0x20)
			disp -= 0x40;
		c->pc = (uint16_t)(c->pc + disp);
		return 10;
	}
	c->illegal = 1;
	return 4;
}

static int
exec_one(upd7810 *c)
{
	uint8_t op = rd(c, c->pc), lf = c->psw & (UPD_L0 | UPD_L1);
	int n;

	c->illegal = 0;
	c->psw &= (uint8_t)~(UPD_L0 | UPD_L1);
	if (c->psw & UPD_SK) {
		int len = upd7810_oplen(c, c->pc);
		c->op_pc = c->pc;
		c->psw &= (uint8_t)~UPD_SK;
		c->pc = (uint16_t)(c->pc + len);
		return skip_states(op, len);
	}
	/* string effect: a run of MVI A, byte or of LXI H, word acts only once */
	if (op == 0x69 && (lf & UPD_L1)) {
		c->op_pc = c->pc;
		c->pc += 2;
		c->psw |= UPD_L1;
		return 7;
	}
	if (op == 0x34 && (lf & UPD_L0)) {
		c->op_pc = c->pc;
		c->pc += 3;
		c->psw |= UPD_L0;
		return 10;
	}
	n = exec(c);
	if (op == 0x69)
		c->psw |= UPD_L1;
	else if (op == 0x34)
		c->psw |= UPD_L0;
	return n;
}

int
upd7810_step(upd7810 *c)
{
	int n;

	if (c->stopped) {
		tick(c, 4);
		return 4;
	}
	if (c->halt) {
		if (!(c->irr & 1u << UPD_FNMI) && !unmasked(c)) {
			tick(c, 4);
			return 4;
		}
		c->halt = 0;
		if ((n = irq(c)) != 0) {
			tick(c, n);
			return n;
		}
	}
	n = exec_one(c);
	tick(c, n);
	if (c->ei_delay)
		c->ei_delay = 0;
	else {
		int m = irq(c);
		if (m) {
			tick(c, m);
			n += m;
		}
	}
	return n;
}

void
upd7810_reset(upd7810 *c)
{
	c->pc = 0;
	c->psw = 0;
	c->ie = 0;
	c->ei_delay = 0;
	c->halt = 0;
	c->stopped = 0;
	c->irr = 0;
	c->mkl = c->mkh = 0xFF;
	c->mode[UPD_PA] = c->mode[UPD_PB] = c->mode[UPD_PC] = c->mode[UPD_PF] = 0xFF;
	c->mcc = 0;
	c->mm &= 0x08;		/* RAE undefined after reset; kept */
	c->tmm = 0xFF;
	c->tff = 0;
	c->etmm = c->eom = 0;
	c->smh = 0;
	c->sml = 0x48;
	c->anm = 0;
	c->zcm = 0xFF;
	c->tx_busy = 0;
	c->txb_full = 0;
	c->rx_state = 0;
	c->rx_full = 0;
	c->ad_idx = 0;
	c->ad_acc = 0;
	c->txd = 1;
	port_changed(c, UPD_PA);
	port_changed(c, UPD_PB);
	port_changed(c, UPD_PC);
}

void
upd7810_init(upd7810 *c, const upd7810_bus *bus)
{
	memset(c, 0, sizeof(*c));
	c->bus = *bus;
	c->rxd = 1;
	c->ci = 1;
	c->txd = 1;
	upd7810_reset(c);
}
