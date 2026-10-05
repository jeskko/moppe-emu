/*
 * Hitachi H8/500 CPU core, maximum mode; see h8500.h.
 */
#include <string.h>

#include "h8500.h"

/* EA kinds */
enum { EA_REG, EA_IMM, EA_MEM };

typedef struct ea_t {
	int      kind;
	int      w;		/* word operand */
	int      n;		/* register */
	uint16_t imm;
	uint32_t a;		/* 24-bit address */
	int      stack;		/* @-R7 / @R7+: always a word access */
} ea_t;

static uint8_t
rd8(h8500 *c, uint32_t a)
{
	a &= 0xFFFFFF;
	c->st += c->bus.states(c->bus.ctx, a);
	return c->bus.read(c->bus.ctx, a);
}

static void
wr8(h8500 *c, uint32_t a, uint8_t v)
{
	a &= 0xFFFFFF;
	c->st += c->bus.states(c->bus.ctx, a);
	c->bus.write(c->bus.ctx, a, v);
}

/* word accesses: upper byte first, which is what the 16-bit on-chip
 * registers' temporary-register protocol expects */
static uint16_t
rd16(h8500 *c, uint32_t a)
{
	uint16_t v;
	if (a & 1)
		c->last_exc = H8_VEC_ADDRERR;
	a &= ~1u;
	v = (uint16_t)(rd8(c, a) << 8);
	return v | rd8(c, (a & 0xFF0000) | ((a + 1) & 0xFFFF));
}

static void
wr16(h8500 *c, uint32_t a, uint16_t v)
{
	if (a & 1)
		c->last_exc = H8_VEC_ADDRERR;
	a &= ~1u;
	wr8(c, a, (uint8_t)(v >> 8));
	wr8(c, (a & 0xFF0000) | ((a + 1) & 0xFFFF), (uint8_t)v);
}

static uint8_t
fetch(h8500 *c)
{
	uint8_t v = rd8(c, (uint32_t)c->cp << 16 | c->pc);
	c->pc++;
	return v;
}

static uint16_t
fetch16(h8500 *c)
{
	uint16_t v = (uint16_t)(fetch(c) << 8);
	return v | fetch(c);
}

static void
push16(h8500 *c, uint16_t v)
{
	c->r[7] -= 2;
	wr16(c, (uint32_t)c->tp << 16 | c->r[7], v);
}

static uint16_t
pop16(h8500 *c)
{
	uint16_t v = rd16(c, (uint32_t)c->tp << 16 | c->r[7]);
	c->r[7] += 2;
	return v;
}

static void
exception(h8500 *c, int vec)
{
	push16(c, c->pc);
	push16(c, c->cp);
	push16(c, c->sr);
	c->sr &= ~H8_T;
	c->cp = (uint8_t)rd16(c, (uint32_t)vec * 4);
	c->pc = rd16(c, (uint32_t)vec * 4 + 2);
	c->last_exc = vec;
	c->st += 4;
}

/* ---------------------------------------------------------------- flags */

static void
nz8(h8500 *c, uint8_t v)
{
	c->sr &= ~(H8_N | H8_Z | H8_V);
	if (!v)
		c->sr |= H8_Z;
	if (v & 0x80)
		c->sr |= H8_N;
}

static void
nz16(h8500 *c, uint16_t v)
{
	c->sr &= ~(H8_N | H8_Z | H8_V);
	if (!v)
		c->sr |= H8_Z;
	if (v & 0x8000)
		c->sr |= H8_N;
}

static void
nz(h8500 *c, int w, uint16_t v)
{
	if (w)
		nz16(c, v);
	else
		nz8(c, (uint8_t)v);
}

/* add/subtract with carry in; ext: Z only cleared (ADDX/SUBX/DADD) */
static uint16_t
addsub(h8500 *c, int w, uint16_t a, uint16_t b, int sub, int cin, int ext)
{
	uint32_t m = w ? 0xFFFF : 0xFF, s = w ? 0x8000 : 0x80, r;
	a &= (uint16_t)m;
	b &= (uint16_t)m;
	r = sub ? (uint32_t)a - b - (uint32_t)cin : (uint32_t)a + b + (uint32_t)cin;
	uint32_t v = sub ? (a ^ b) & (a ^ r) : ~(a ^ b) & (a ^ r);
	uint16_t sr = c->sr & ~(H8_N | H8_V | H8_C);
	if (ext) {
		if (r & m)
			sr &= ~H8_Z;
	} else {
		sr &= ~H8_Z;
		if (!(r & m))
			sr |= H8_Z;
	}
	if (r & s)
		sr |= H8_N;
	if (v & s)
		sr |= H8_V;
	if (r & (m + 1))
		sr |= H8_C;
	c->sr = sr;
	return (uint16_t)(r & m);
}

static uint16_t
shift(h8500 *c, int w, int op, uint16_t v)
{
	uint16_t m = w ? 0xFFFF : 0xFF, s = w ? 0x8000 : 0x80;
	int cy = (c->sr & H8_C) != 0, out, ov = 0;
	uint16_t r;
	v &= m;
	switch (op) {
	case 0x18:	/* SHAL */
		out = (v & s) != 0;
		ov = ((v ^ (v << 1)) & s) != 0;
		r = (uint16_t)(v << 1);
		break;
	case 0x19:	/* SHAR */
		out = v & 1;
		r = (uint16_t)((v >> 1) | (v & s));
		break;
	case 0x1A:	/* SHLL */
		out = (v & s) != 0;
		r = (uint16_t)(v << 1);
		break;
	case 0x1B:	/* SHLR */
		out = v & 1;
		r = v >> 1;
		break;
	case 0x1C:	/* ROTL */
		out = (v & s) != 0;
		r = (uint16_t)((v << 1) | out);
		break;
	case 0x1D:	/* ROTR */
		out = v & 1;
		r = (uint16_t)((v >> 1) | (out ? s : 0));
		break;
	case 0x1E:	/* ROTXL */
		out = (v & s) != 0;
		r = (uint16_t)((v << 1) | cy);
		break;
	default:	/* ROTXR */
		out = v & 1;
		r = (uint16_t)((v >> 1) | (cy ? s : 0));
		break;
	}
	r &= m;
	nz(c, w, r);
	c->sr &= ~H8_C;
	if (out)
		c->sr |= H8_C;
	if (ov)
		c->sr |= H8_V;
	return r;
}

static int
cond(h8500 *c, int cc)
{
	int C = (c->sr & H8_C) != 0, V = (c->sr & H8_V) != 0;
	int Z = (c->sr & H8_Z) != 0, N = (c->sr & H8_N) != 0;
	switch (cc & 15) {
	case 0: return 1;
	case 1: return 0;
	case 2: return !(C | Z);
	case 3: return C | Z;
	case 4: return !C;
	case 5: return C;
	case 6: return !Z;
	case 7: return Z;
	case 8: return !V;
	case 9: return V;
	case 10: return !N;
	case 11: return N;
	case 12: return !(N ^ V);
	case 13: return N ^ V;
	case 14: return !(Z | (N ^ V));
	default: return Z | (N ^ V);
	}
}

/* ------------------------------------------------------------------ EA */

static uint8_t
page_of(h8500 *c, int n)
{
	return n < 4 ? c->dp : n < 6 ? c->ep : c->tp;
}

/* decode the EA prefix e (already fetched); 0 if e is not one */
static int
ea_decode(h8500 *c, uint8_t e, ea_t *x)
{
	int n = e & 7, step;
	memset(x, 0, sizeof(*x));
	x->w = (e >> 3) & 1;
	switch (e) {
	case 0x04:
		x->kind = EA_IMM;
		x->imm = fetch(c);
		return 1;
	case 0x0C:
		x->kind = EA_IMM;
		x->imm = fetch16(c);
		return 1;
	case 0x05: case 0x0D:
		x->kind = EA_MEM;
		x->a = (uint32_t)c->br << 8 | fetch(c);
		c->st += 1;
		return 1;
	case 0x15: case 0x1D:
		x->kind = EA_MEM;
		x->a = (uint32_t)c->dp << 16 | fetch16(c);
		c->st += 1;
		return 1;
	}
	if (e < 0xA0)
		return 0;
	x->n = n;
	step = (x->w || n == 7) ? 2 : 1;
	x->stack = (n == 7 && (e & 0xF0) != 0xA0 && (e & 0xF0) != 0xD0
	    && (e & 0xF0) != 0xE0 && (e & 0xF0) != 0xF0);
	switch (e & 0xF0) {
	case 0xA0:
		x->kind = EA_REG;
		return 1;
	case 0xB0:	/* @-Rn */
		c->r[n] -= (uint16_t)step;
		x->a = (uint32_t)page_of(c, n) << 16 | c->r[n];
		c->st += 2;
		break;
	case 0xC0:	/* @Rn+ */
		x->a = (uint32_t)page_of(c, n) << 16 | c->r[n];
		c->r[n] += (uint16_t)step;
		c->st += 3;
		break;
	case 0xD0:
		x->a = (uint32_t)page_of(c, n) << 16 | c->r[n];
		c->st += 2;
		break;
	case 0xE0: {
		int8_t d = (int8_t)fetch(c);
		x->a = (uint32_t)page_of(c, n) << 16 | (uint16_t)(c->r[n] + d);
		c->st += 1;
		break;
	}
	default: {
		uint16_t d = fetch16(c);
		x->a = (uint32_t)page_of(c, n) << 16 | (uint16_t)(c->r[n] + d);
		c->st += 1;
		break;
	}
	}
	x->kind = EA_MEM;
	return 1;
}

static uint16_t
ea_rd(h8500 *c, ea_t *x)
{
	switch (x->kind) {
	case EA_REG:
		return x->w ? c->r[x->n] : c->r[x->n] & 0xFF;
	case EA_IMM:
		return x->w ? x->imm : x->imm & 0xFF;
	default:
		if (x->w)
			return rd16(c, x->a);
		if (x->stack)	/* a byte in the stack is the odd byte of a word */
			return rd16(c, x->a) & 0xFF;
		return rd8(c, x->a);
	}
}

static void
ea_wr(h8500 *c, ea_t *x, uint16_t v)
{
	switch (x->kind) {
	case EA_REG:
		if (x->w)
			c->r[x->n] = v;
		else
			c->r[x->n] = (uint16_t)((c->r[x->n] & 0xFF00) | (v & 0xFF));
		return;
	case EA_IMM:
		c->illegal = 1;
		return;
	default:
		if (x->w)
			wr16(c, x->a, v);
		else if (x->stack)
			wr16(c, x->a, v & 0xFF);
		else
			wr8(c, x->a, (uint8_t)v);
	}
}

static uint16_t
rget(h8500 *c, int n, int w)
{
	return w ? c->r[n & 7] : c->r[n & 7] & 0xFF;
}

static void
rset(h8500 *c, int n, int w, uint16_t v)
{
	n &= 7;
	if (w)
		c->r[n] = v;
	else
		c->r[n] = (uint16_t)((c->r[n] & 0xFF00) | (v & 0xFF));
}

/* ------------------------------------------------------- control regs */

static int
ctl_get(h8500 *c, int cr, int w, uint16_t *v)
{
	if (w) {
		if (cr != 0)
			return 0;
		*v = c->sr;
		return 1;
	}
	switch (cr) {
	case 1: *v = c->sr & 0xFF; return 1;
	case 3: *v = c->br; return 1;
	case 4: *v = c->ep; return 1;
	case 5: *v = c->dp; return 1;
	case 7: *v = c->tp; return 1;
	}
	return 0;
}

static int
ctl_set(h8500 *c, int cr, int w, uint16_t v)
{
	if (w) {
		if (cr != 0)
			return 0;
		c->sr = v & H8_SR_MASK;
		return 1;
	}
	switch (cr) {
	case 1: c->sr = (uint16_t)((c->sr & 0xFF00) | (v & 0x0F)); return 1;
	case 3: c->br = (uint8_t)v; return 1;
	case 4: c->ep = (uint8_t)v; return 1;
	case 5: c->dp = (uint8_t)v; return 1;
	case 7: c->tp = (uint8_t)v; return 1;
	}
	return 0;
}

/* -------------------------------------------------------------- execute */

static void
invalid(h8500 *c)
{
	c->illegal = 1;
}

static void
branch(h8500 *c, uint16_t target)
{
	c->pc = target;
	c->st += 3 + (target & 1);
}

/* ops following an EA prefix */
static void
exec_ea(h8500 *c, uint8_t e)
{
	ea_t x;
	uint8_t op;
	uint16_t a, b, r;
	int w, rn, cr;

	ea_decode(c, e, &x);
	op = fetch(c);
	w = x.w;
	rn = op & 7;

	switch (op & 0xF8) {
	case 0x00:
		switch (op) {
		case 0x00: {	/* MOVFPE / MOVTPE / DADD / DSUB */
			uint8_t o3 = fetch(c);
			switch (o3 & 0xF8) {
			case 0x80:	/* MOVFPE <EA>, Rd */
				r = ea_rd(c, &x);
				rset(c, o3, 0, r);
				nz8(c, (uint8_t)r);
				c->st += 6;
				return;
			case 0x90:	/* MOVTPE Rs, <EA> */
				r = rget(c, o3, 0);
				ea_wr(c, &x, r);
				nz8(c, (uint8_t)r);
				c->st += 6;
				return;
			case 0xA0:	/* DADD Rs, Rd */
			case 0xB0: {	/* DSUB Rs, Rd */
				int sub = (o3 & 0xF8) == 0xB0, cy = (c->sr & H8_C) != 0;
				int s = c->r[x.n] & 0xFF, d = c->r[o3 & 7] & 0xFF, lo, hi, res;
				if (x.kind != EA_REG)
					break;
				if (!sub) {
					lo = (d & 15) + (s & 15) + cy;
					hi = (d >> 4) + (s >> 4);
					if (lo > 9) { lo -= 10; hi++; }
					cy = hi > 9;
					if (cy)
						hi -= 10;
				} else {
					lo = (d & 15) - (s & 15) - cy;
					hi = (d >> 4) - (s >> 4);
					if (lo < 0) { lo += 10; hi--; }
					cy = hi < 0;
					if (cy)
						hi += 10;
				}
				res = (hi << 4 | lo) & 0xFF;
				rset(c, o3, 0, (uint16_t)res);
				c->sr &= ~H8_C;
				if (cy)
					c->sr |= H8_C;
				if (res)
					c->sr &= ~H8_Z;
				c->st += 2;
				return;
			}
			}
			invalid(c);
			return;
		}
		case 0x04:	/* CMP:G #xx:8, <EA> */
			b = fetch(c);
			if (w)
				b = (uint16_t)(int16_t)(int8_t)b;
			addsub(c, w, ea_rd(c, &x), b, 1, 0, 0);
			return;
		case 0x05:	/* CMP:G #xx:16, <EA> */
			b = fetch16(c);
			addsub(c, w, ea_rd(c, &x), b, 1, 0, 0);
			return;
		case 0x06:	/* MOV:G #xx:8, <EA> */
			b = fetch(c);
			if (w)
				b = (uint16_t)(int16_t)(int8_t)b;
			ea_wr(c, &x, b);
			nz(c, w, b);
			return;
		case 0x07:	/* MOV:G #xx:16, <EA> */
			b = fetch16(c);
			ea_wr(c, &x, b);
			nz(c, w, b);
			return;
		}
		invalid(c);
		return;
	case 0x08:	/* ADD:Q #±1/±2, <EA> */
		if ((op & 7) > 5 || (op & 7) == 2 || (op & 7) == 3) {
			invalid(c);
			return;
		}
		a = ea_rd(c, &x);
		b = (uint16_t)((op & 1) + 1);
		r = (op & 4) ? addsub(c, w, a, b, 1, 0, 0) : addsub(c, w, a, b, 0, 0, 0);
		ea_wr(c, &x, r);
		c->st += 1;
		return;
	case 0x10:
	case 0x18:
		switch (op) {
		case 0x10:	/* SWAP Rd */
			if (x.kind != EA_REG)
				break;
			r = (uint16_t)(c->r[x.n] << 8 | c->r[x.n] >> 8);
			c->r[x.n] = r;
			nz16(c, r);
			return;
		case 0x11:	/* EXTS Rd */
		case 0x12:	/* EXTU Rd */
			if (x.kind != EA_REG)
				break;
			r = c->r[x.n] & 0xFF;
			if (op == 0x11)
				r = (uint16_t)(int16_t)(int8_t)r;
			c->r[x.n] = r;
			nz16(c, r);
			c->sr &= ~H8_C;
			return;
		case 0x13:	/* CLR */
			ea_wr(c, &x, 0);
			c->sr = (uint16_t)((c->sr & ~(H8_N | H8_V | H8_C)) | H8_Z);
			return;
		case 0x14:	/* NEG */
			r = addsub(c, w, 0, ea_rd(c, &x), 1, 0, 0);
			ea_wr(c, &x, r);
			c->st += 1;
			return;
		case 0x15:	/* NOT */
			r = (uint16_t)~ea_rd(c, &x);
			if (!w)
				r &= 0xFF;
			ea_wr(c, &x, r);
			nz(c, w, r);
			c->st += 1;
			return;
		case 0x16:	/* TST */
			nz(c, w, ea_rd(c, &x));
			c->sr &= ~H8_C;
			return;
		case 0x17:	/* TAS */
			r = ea_rd(c, &x);
			nz(c, 0, r);
			c->sr &= ~H8_C;
			ea_wr(c, &x, r | 0x80);
			c->st += 3;
			return;
		default:
			r = shift(c, w, op, ea_rd(c, &x));
			ea_wr(c, &x, r);
			c->st += 1;
			return;
		}
		invalid(c);
		return;
	case 0x20:	/* ADD:G <EA>, Rd */
		rset(c, rn, w, addsub(c, w, rget(c, rn, w), ea_rd(c, &x), 0, 0, 0));
		return;
	case 0x28:	/* ADDS <EA>, Rd */
		b = ea_rd(c, &x);
		if (!w)
			b = (uint16_t)(int16_t)(int8_t)b;
		c->r[rn] += b;
		c->st += 1;
		return;
	case 0x30:	/* SUB <EA>, Rd */
		rset(c, rn, w, addsub(c, w, rget(c, rn, w), ea_rd(c, &x), 1, 0, 0));
		return;
	case 0x38:	/* SUBS <EA>, Rd */
		b = ea_rd(c, &x);
		if (!w)
			b = (uint16_t)(int16_t)(int8_t)b;
		c->r[rn] -= b;
		c->st += 1;
		return;
	case 0x40:	/* OR */
		r = rget(c, rn, w) | ea_rd(c, &x);
		rset(c, rn, w, r);
		nz(c, w, r);
		return;
	case 0x50:	/* AND */
		r = rget(c, rn, w) & ea_rd(c, &x);
		rset(c, rn, w, r);
		nz(c, w, r);
		return;
	case 0x60:	/* XOR */
		r = rget(c, rn, w) ^ ea_rd(c, &x);
		rset(c, rn, w, r);
		nz(c, w, r);
		return;
	case 0x48:	/* BSET Rn, <EA> / ORC #xx, CR */
	case 0x58:	/* BCLR Rn, <EA> / ANDC #xx, CR */
	case 0x68:	/* BNOT Rn, <EA> / XORC #xx, CR */
		if (x.kind == EA_IMM) {
			uint16_t v;
			cr = rn;
			if (!ctl_get(c, cr, w, &v)) {
				invalid(c);
				return;
			}
			b = x.w ? x.imm : x.imm & 0xFF;
			v = (op & 0xF8) == 0x48 ? v | b : (op & 0xF8) == 0x58 ? v & b : v ^ b;
			ctl_set(c, cr, w, v);
			if (cr != 0 && cr != 1)
				nz8(c, (uint8_t)v);
			c->irq_inhibit = 1;
			c->st += w ? 5 : 2;
			return;
		} else {
			int bit = c->r[rn] & (w ? 15 : 7);
			a = ea_rd(c, &x);
			c->sr &= ~H8_Z;
			if (!(a & (1u << bit)))
				c->sr |= H8_Z;
			if ((op & 0xF8) == 0x48)
				a |= (uint16_t)(1u << bit);
			else if ((op & 0xF8) == 0x58)
				a &= (uint16_t)~(1u << bit);
			else
				a ^= (uint16_t)(1u << bit);
			ea_wr(c, &x, a);
			c->st += 2;
			return;
		}
	case 0x70:	/* CMP:G <EA>, Rd */
		addsub(c, w, rget(c, rn, w), ea_rd(c, &x), 1, 0, 0);
		return;
	case 0x78: {	/* BTST Rn, <EA> */
		int bit = c->r[rn] & (w ? 15 : 7);
		a = ea_rd(c, &x);
		c->sr &= ~H8_Z;
		if (!(a & (1u << bit)))
			c->sr |= H8_Z;
		c->st += 1;
		return;
	}
	case 0x80:	/* MOV:G <EA>, Rd */
		r = ea_rd(c, &x);
		rset(c, rn, w, r);
		nz(c, w, r);
		return;
	case 0x88:	/* LDC <EA>, CR */
		cr = rn;
		if (!w && cr == 4 && x.stack && x.kind == EA_MEM && (e & 0xF0) == 0xC0) {
			/* byte LDC @R7+, EP pops the EP:DP pair (see h8500.h) */
			r = rd16(c, x.a);
			c->ep = (uint8_t)(r >> 8);
			c->dp = (uint8_t)r;
		} else if (!ctl_set(c, cr, w, ea_rd(c, &x))) {
			invalid(c);
			return;
		}
		c->irq_inhibit = 1;
		c->st += w ? 2 : 1;
		return;
	case 0x90:
		if (x.kind == EA_REG && w) {	/* XCH Rs, Rd */
			r = c->r[x.n];
			c->r[x.n] = c->r[rn];
			c->r[rn] = r;
			c->st += 2;
			return;
		}
		/* MOV:G Rs, <EA>; @-Rn with Rs = Rn stores the decremented value */
		r = rget(c, rn, w);
		ea_wr(c, &x, r);
		nz(c, w, r);
		return;
	case 0x98: {	/* STC CR, <EA> */
		uint16_t v;
		cr = rn;
		if (x.kind == EA_IMM) {
			invalid(c);
			return;
		}
		if (!w && cr == 4 && x.stack && (e & 0xF0) == 0xB0) {
			/* byte STC EP, @-R7 pushes the EP:DP pair */
			wr16(c, x.a, (uint16_t)(c->ep << 8 | c->dp));
		} else {
			if (!ctl_get(c, cr, w, &v)) {
				invalid(c);
				return;
			}
			ea_wr(c, &x, v);
		}
		c->st += 2;
		return;
	}
	case 0xA0:	/* ADDX */
		rset(c, rn, w, addsub(c, w, rget(c, rn, w), ea_rd(c, &x), 0,
		    (c->sr & H8_C) != 0, 1));
		return;
	case 0xB0:	/* SUBX */
		rset(c, rn, w, addsub(c, w, rget(c, rn, w), ea_rd(c, &x), 1,
		    (c->sr & H8_C) != 0, 1));
		return;
	case 0xA8:	/* MULXU */
		b = ea_rd(c, &x);
		if (w) {
			uint32_t p = (uint32_t)c->r[rn] * b;
			c->r[rn] = (uint16_t)(p >> 16);
			c->r[(rn + 1) & 7] = (uint16_t)p;
			c->sr &= ~(H8_N | H8_Z | H8_V | H8_C);
			if (!p)
				c->sr |= H8_Z;
			if (p & 0x80000000u)
				c->sr |= H8_N;
			c->st += 20;
		} else {
			uint16_t p = (uint16_t)((c->r[rn] & 0xFF) * b);
			c->r[rn] = p;
			c->sr &= ~(H8_N | H8_Z | H8_V | H8_C);
			if (!p)
				c->sr |= H8_Z;
			if (p & 0x8000)
				c->sr |= H8_N;
			c->st += 14;
		}
		return;
	case 0xB8:	/* DIVXU */
		b = ea_rd(c, &x);
		if (!b) {
			c->sr = (uint16_t)((c->sr & ~(H8_N | H8_V | H8_C)) | H8_Z);
			c->st += 10;
			exception(c, H8_VEC_ZERODIV);
			return;
		}
		if (w) {
			uint32_t d = (uint32_t)c->r[rn] << 16 | c->r[(rn + 1) & 7];
			if ((d >> 16) >= b) {
				c->sr = (uint16_t)((c->sr & ~(H8_N | H8_Z | H8_C)) | H8_V);
				c->st += 6;
				return;
			}
			c->r[rn] = (uint16_t)(d % b);
			c->r[(rn + 1) & 7] = (uint16_t)(d / b);
			nz16(c, (uint16_t)(d / b));
			c->sr &= ~H8_C;
			c->st += 24;
		} else {
			uint16_t d = c->r[rn];
			if ((d >> 8) >= b) {
				c->sr = (uint16_t)((c->sr & ~(H8_N | H8_Z | H8_C)) | H8_V);
				c->st += 6;
				return;
			}
			c->r[rn] = (uint16_t)((d % b) << 8 | (d / b));
			nz8(c, (uint8_t)(d / b));
			c->sr &= ~H8_C;
			c->st += 18;
		}
		return;
	}

	/* bit ops with an immediate bit number: C0 BSET, D0 BCLR, E0 BNOT, F0 BTST */
	{
		int bit = op & (w ? 15 : 7), kind = op & 0xF0;
		if (!w && (op & 8)) {
			invalid(c);
			return;
		}
		a = ea_rd(c, &x);
		c->sr &= ~H8_Z;
		if (!(a & (1u << bit)))
			c->sr |= H8_Z;
		if (kind == 0xF0) {
			c->st += 1;
			return;
		}
		if (kind == 0xC0)
			a |= (uint16_t)(1u << bit);
		else if (kind == 0xD0)
			a &= (uint16_t)~(1u << bit);
		else
			a ^= (uint16_t)(1u << bit);
		ea_wr(c, &x, a);
		c->st += 2;
	}
}

static void
exec(h8500 *c)
{
	uint8_t op = fetch(c), b;
	int16_t d;
	uint16_t t;
	int i;

	switch (op) {
	case 0x00:	/* NOP */
		c->st += 1;
		return;
	case 0x01: case 0x06: case 0x07:	/* SCB/F, SCB/NE, SCB/EQ */
		b = fetch(c);
		d = (int8_t)fetch(c);
		if ((b & 0xF8) != 0xB8) {
			invalid(c);
			return;
		}
		c->st += 1;
		if ((op == 0x06 && !(c->sr & H8_Z)) || (op == 0x07 && (c->sr & H8_Z)))
			return;
		if (--c->r[b & 7] != 0xFFFF)
			branch(c, (uint16_t)(c->pc + d));
		return;
	case 0x02:	/* LDM @SP+, list */
		b = fetch(c);
		for (i = 0; i < 8; i++)
			if (b & (1 << i)) {
				t = pop16(c);
				if (i != 7)
					c->r[i] = t;
				c->st += 1;
			}
		c->st += 4;
		return;
	case 0x12:	/* STM list, @-SP */
		b = fetch(c);
		t = c->r[7];
		for (i = 7; i >= 0; i--)
			if (b & (1 << i)) {
				push16(c, i == 7 ? (uint16_t)(t - 2) : c->r[i]);
				c->st += 1;
			}
		c->st += 4;
		return;
	case 0x03: {	/* PJSR @aa:24 */
		uint8_t p = fetch(c);
		t = fetch16(c);
		push16(c, c->pc);
		push16(c, c->cp);
		c->cp = p;
		branch(c, t);
		c->st += 4;
		return;
	}
	case 0x13: {	/* PJMP @aa:24 */
		uint8_t p = fetch(c);
		t = fetch16(c);
		c->cp = p;
		branch(c, t);
		return;
	}
	case 0x08:	/* TRAPA #n */
		b = fetch(c);
		if ((b & 0xF0) != 0x10) {
			invalid(c);
			return;
		}
		exception(c, H8_VEC_TRAPA + (b & 15));
		c->irq_inhibit = 1;
		c->st += 8;
		return;
	case 0x09:	/* TRAP/VS */
		c->st += 2;
		if (c->sr & H8_V) {
			exception(c, H8_VEC_TRAPVS);
			c->irq_inhibit = 1;
			c->st += 8;
		}
		return;
	case 0x0A:	/* RTE */
		c->sr = pop16(c) & H8_SR_MASK;
		c->cp = (uint8_t)pop16(c);
		c->pc = pop16(c);
		c->irq_inhibit = 1;
		c->st += 8;
		return;
	case 0x0E:	/* BSR d:8 */
		d = (int8_t)fetch(c);
		push16(c, c->pc);
		branch(c, (uint16_t)(c->pc + d));
		c->st += 2;
		return;
	case 0x1E:	/* BSR d:16 */
		d = (int16_t)fetch16(c);
		push16(c, c->pc);
		branch(c, (uint16_t)(c->pc + d));
		c->st += 1;
		return;
	case 0x0F:	/* UNLK FP */
		c->r[7] = c->r[6];
		c->r[6] = pop16(c);
		c->st += 3;
		return;
	case 0x17:	/* LINK FP, #d:8 */
	case 0x1F:	/* LINK FP, #d:16 */
		d = op == 0x17 ? (int8_t)fetch(c) : (int16_t)fetch16(c);
		push16(c, c->r[6]);
		c->r[6] = c->r[7];
		c->r[7] += (uint16_t)d;
		c->st += 3;
		return;
	case 0x10:	/* JMP @aa:16 */
		branch(c, fetch16(c));
		return;
	case 0x18:	/* JSR @aa:16 */
		t = fetch16(c);
		push16(c, c->pc);
		branch(c, t);
		c->st += 1;
		return;
	case 0x14:	/* RTD #d:8 */
	case 0x1C:	/* RTD #d:16 */
		d = op == 0x14 ? (int8_t)fetch(c) : (int16_t)fetch16(c);
		t = pop16(c);
		c->r[7] += (uint16_t)d;
		branch(c, t);
		c->st += 3;
		return;
	case 0x19:	/* RTS */
		branch(c, pop16(c));
		c->st += 3;
		return;
	case 0x1A:	/* SLEEP */
		c->sleeping = 1;
		return;
	case 0x11:
		b = fetch(c);
		switch (b) {
		case 0x14:	/* PRTD #d:8 */
		case 0x1C:	/* PRTD #d:16 */
			d = b == 0x14 ? (int8_t)fetch(c) : (int16_t)fetch16(c);
			c->cp = (uint8_t)pop16(c);
			t = pop16(c);
			c->r[7] += (uint16_t)d;
			branch(c, t);
			c->st += 4;
			return;
		case 0x19:	/* PRTS */
			c->cp = (uint8_t)pop16(c);
			branch(c, pop16(c));
			c->st += 4;
			return;
		}
		switch (b & 0xF8) {
		case 0xC0:	/* PJMP @Rn: CP from Rn, PC from Rn+1 */
			c->cp = (uint8_t)c->r[b & 7];
			branch(c, c->r[(b + 1) & 7]);
			c->st += 2;
			return;
		case 0xC8: {	/* PJSR @Rn */
			uint8_t p = (uint8_t)c->r[b & 7];
			t = c->r[(b + 1) & 7];
			push16(c, c->pc);
			push16(c, c->cp);
			c->cp = p;
			branch(c, t);
			c->st += 4;
			return;
		}
		case 0xD0:	/* JMP @Rn */
			branch(c, c->r[b & 7]);
			return;
		case 0xD8:	/* JSR @Rn */
			t = c->r[b & 7];
			push16(c, c->pc);
			branch(c, t);
			c->st += 2;
			return;
		case 0xE0:	/* JMP @(d:8, Rn) */
		case 0xE8:	/* JSR @(d:8, Rn) */
		case 0xF0:	/* JMP @(d:16, Rn) */
		case 0xF8:	/* JSR @(d:16, Rn) */
			d = (b & 0x10) ? (int16_t)fetch16(c) : (int8_t)fetch(c);
			t = (uint16_t)(c->r[b & 7] + d);
			if (b & 8)
				push16(c, c->pc);
			branch(c, t);
			c->st += 1;
			return;
		}
		invalid(c);
		return;
	}

	switch (op & 0xF0) {
	case 0x20:	/* Bcc d:8 */
		d = (int8_t)fetch(c);
		c->st += 1;
		if (cond(c, op))
			branch(c, (uint16_t)(c->pc + d));
		return;
	case 0x30:	/* Bcc d:16 */
		d = (int16_t)fetch16(c);
		if (cond(c, op))
			branch(c, (uint16_t)(c->pc + d));
		return;
	case 0x40:
		if (op & 8)	/* CMP:I #xx:16, Rd */
			addsub(c, 1, c->r[op & 7], fetch16(c), 1, 0, 0);
		else		/* CMP:E #xx:8, Rd */
			addsub(c, 0, c->r[op & 7], fetch(c), 1, 0, 0);
		return;
	case 0x50:
		if (op & 8) {	/* MOV:I #xx:16, Rd */
			t = fetch16(c);
			c->r[op & 7] = t;
			nz16(c, t);
		} else {	/* MOV:E #xx:8, Rd */
			b = fetch(c);
			rset(c, op, 0, b);
			nz8(c, b);
		}
		return;
	case 0x60:	/* MOV:L @aa:8, Rd */
	case 0x70: {	/* MOV:S Rs, @aa:8 */
		int w = (op >> 3) & 1;
		uint32_t a = (uint32_t)c->br << 8 | fetch(c);
		if ((op & 0xF0) == 0x60) {
			t = w ? rd16(c, a) : rd8(c, a);
			rset(c, op, w, t);
		} else {
			t = rget(c, op, w);
			if (w)
				wr16(c, a, t);
			else
				wr8(c, a, (uint8_t)t);
		}
		nz(c, w, t);
		c->st += 1;
		return;
	}
	case 0x80:	/* MOV:F @(d:8, R6), Rd */
	case 0x90: {	/* MOV:F Rs, @(d:8, R6) */
		int w = (op >> 3) & 1;
		uint32_t a;
		d = (int8_t)fetch(c);
		a = (uint32_t)c->tp << 16 | (uint16_t)(c->r[6] + d);
		if ((op & 0xF0) == 0x80) {
			t = w ? rd16(c, a) : rd8(c, a);
			rset(c, op, w, t);
		} else {
			t = rget(c, op, w);
			if (w)
				wr16(c, a, t);
			else
				wr8(c, a, (uint8_t)t);
		}
		nz(c, w, t);
		c->st += 1;
		return;
	}
	}

	if (op == 0x04 || op == 0x05 || op == 0x0C || op == 0x0D
	    || op == 0x15 || op == 0x1D || op >= 0xA0) {
		exec_ea(c, op);
		return;
	}
	invalid(c);
}

void
h8500_init(h8500 *c, const h8500_bus *bus)
{
	memset(c, 0, sizeof(*c));
	c->bus = *bus;
	c->last_exc = -1;
}

void
h8500_reset(h8500 *c)
{
	c->cp = c->dp = c->ep = c->tp = c->br = 0;
	c->sr = H8_I;
	c->sleeping = 0;
	c->irq_inhibit = 0;
	c->illegal = 0;
	c->st = 0;
	c->cp = (uint8_t)rd16(c, 0);
	c->pc = rd16(c, 2);
	c->states += (uint64_t)c->st;
	c->last_exc = H8_VEC_RESET;
}

void
h8500_irq(h8500 *c, int vec, int level)
{
	c->irq_vec = vec;
	c->irq_level = level;
}

uint32_t
h8500_pc24(const h8500 *c)
{
	return (uint32_t)c->cp << 16 | c->pc;
}

int
h8500_step(h8500 *c)
{
	int mask = (c->sr & H8_I) >> 8;

	c->st = 0;
	c->illegal = 0;
	c->last_exc = -1;
	if (c->irq_vec && !c->irq_inhibit && (c->irq_level > mask || c->irq_level == 8)) {
		int lvl = c->irq_level > 7 ? 7 : c->irq_level;
		c->sleeping = 0;
		c->op_addr = h8500_pc24(c);
		c->st += 2;
		exception(c, c->irq_vec);
		c->sr = (uint16_t)((c->sr & ~H8_I) | lvl << 8);
		c->irq_vec = 0;		/* the chip presents the next one */
		c->states += (uint64_t)c->st;
		return c->st;
	}
	c->irq_inhibit = 0;
	if (c->sleeping) {
		c->states += 2;
		return 2;
	}
	c->op_addr = h8500_pc24(c);
	exec(c);
	if (c->illegal) {
		c->pc = (uint16_t)c->op_addr;
		c->illegal = 0;
		exception(c, H8_VEC_INVALID);
		c->illegal = 1;
	} else if (c->last_exc == H8_VEC_ADDRERR) {
		exception(c, H8_VEC_ADDRERR);
	}
	c->states += (uint64_t)c->st;
	return c->st;
}

/* --------------------------------------------------- instruction length */

static int
ea_len(uint8_t e)
{
	switch (e) {
	case 0x04: case 0x05: case 0x0D: return 2;
	case 0x0C: case 0x15: case 0x1D: return 3;
	}
	if ((e & 0xF0) == 0xE0)
		return 2;
	if ((e & 0xF0) == 0xF0)
		return 3;
	return 1;
}

int
h8500_oplen(h8500 *c, uint32_t a)
{
	uint8_t op = c->bus.read(c->bus.ctx, a), op2, b;
	uint32_t pg = a & 0xFF0000;
	int n;
#define AT(k) c->bus.read(c->bus.ctx, pg | ((a + (k)) & 0xFFFF))

	switch (op) {
	case 0x01: case 0x06: case 0x07: return 3;
	case 0x02: case 0x12: case 0x08: case 0x0E: case 0x14: case 0x17: return 2;
	case 0x03: case 0x13: return 4;
	case 0x10: case 0x18: case 0x1C: case 0x1E: case 0x1F: return 3;
	case 0x11:
		b = AT(1);
		if (b == 0x14 || (b & 0xF0) == 0xE0)
			return 3;
		if (b == 0x1C || (b & 0xF0) == 0xF0)
			return 4;
		return 2;
	}
	switch (op & 0xF0) {
	case 0x20: return 2;
	case 0x30: return 3;
	case 0x40: case 0x50: return (op & 8) ? 3 : 2;
	case 0x60: case 0x70: case 0x80: case 0x90: return 2;
	}
	if (!(op == 0x04 || op == 0x05 || op == 0x0C || op == 0x0D
	    || op == 0x15 || op == 0x1D || op >= 0xA0))
		return 1;
	n = ea_len(op);
	op2 = AT(n);
	n++;
	switch (op2) {
	case 0x00: return n + 1;
	case 0x04: case 0x06: return n + 1;
	case 0x05: case 0x07: return n + 2;
	}
	return n;
#undef AT
}
