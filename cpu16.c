/*
 * Motorola CPU16 core; see cpu16.h.
 */
#include <stdlib.h>
#include <string.h>

#include "cpu16.h"
#include "cpu16tab.h"

#define A20(a)	((uint32_t)(a) & 0xFFFFF)
#define AM_MASK	((1LL << 36) - 1)

static int16_t  sx8(uint8_t v)   { return (int16_t)(int8_t)v; }
static int32_t  sx16(uint16_t v) { return (int32_t)(int16_t)v; }
static int32_t  sx20(uint32_t v) { return (v & 0x80000) ? (int32_t)(v | 0xFFF00000u) : (int32_t)v; }
static int64_t  sx36(int64_t v)  { v &= AM_MASK; return (v & (1LL << 35)) ? v - (1LL << 36) : v; }

static uint8_t  ga(cpu16 *c) { return (uint8_t)(c->d >> 8); }
static uint8_t  gb(cpu16 *c) { return (uint8_t)c->d; }
static void     sa(cpu16 *c, uint8_t v) { c->d = (uint16_t)((c->d & 0x00FF) | v << 8); }
static void     sb(cpu16 *c, uint8_t v) { c->d = (uint16_t)((c->d & 0xFF00) | v); }

/* ---------------------------------------------------------------- bus */

static int
extra(cpu16 *c, uint32_t a, int size)
{
	int n = c->bus.clocks(c->bus.ctx, a, size) - 2;
	return n > 0 ? n : 0;
}

static uint8_t
rd8(cpu16 *c, uint32_t a)
{
	a = A20(a);
	c->st += extra(c, a, 1);
	return c->bus.rd8(c->bus.ctx, a);
}

static void
wr8(cpu16 *c, uint32_t a, uint8_t v)
{
	a = A20(a);
	c->st += extra(c, a, 1);
	c->bus.wr8(c->bus.ctx, a, v);
}

/* a misaligned word is two byte transfers (3.5.5.3); the table's count
 * assumes one two-clock cycle, so both bytes are extra beyond that */
static uint16_t
rd16(cpu16 *c, uint32_t a)
{
	a = A20(a);
	if (a & 1) {
		uint32_t b = A20(a + 1);
		c->st += c->bus.clocks(c->bus.ctx, a, 1) + c->bus.clocks(c->bus.ctx, b, 1) - 2;
		return (uint16_t)(c->bus.rd8(c->bus.ctx, a) << 8 | c->bus.rd8(c->bus.ctx, b));
	}
	c->st += extra(c, a, 2);
	return c->bus.rd16(c->bus.ctx, a);
}

static void
wr16(cpu16 *c, uint32_t a, uint16_t v)
{
	a = A20(a);
	if (a & 1) {
		uint32_t b = A20(a + 1);
		c->st += c->bus.clocks(c->bus.ctx, a, 1) + c->bus.clocks(c->bus.ctx, b, 1) - 2;
		c->bus.wr8(c->bus.ctx, a, (uint8_t)(v >> 8));
		c->bus.wr8(c->bus.ctx, b, (uint8_t)v);
		return;
	}
	c->st += extra(c, a, 2);
	c->bus.wr16(c->bus.ctx, a, v);
}

static uint32_t sp20(cpu16 *c) { return (uint32_t)c->sk << 16 | c->sp; }

static void
sp_add(cpu16 *c, int n)
{
	uint32_t v = A20(sp20(c) + (uint32_t)n);
	c->sk = (uint8_t)(v >> 16);
	c->sp = (uint16_t)v;
}

/* push: write at SK:SP, then SK:SP - 2; pull: SK:SP + 2, then read */
static void
push16(cpu16 *c, uint16_t v)
{
	wr16(c, sp20(c), v);
	sp_add(c, -2);
}

static uint16_t
pull16(cpu16 *c)
{
	sp_add(c, 2);
	return rd16(c, sp20(c));
}

uint32_t
cpu16_pc20(const cpu16 *c)
{
	return (uint32_t)c->pk << 16 | c->pc;
}

uint16_t
cpu16_ccr(const cpu16 *c)
{
	return (uint16_t)((c->ccr & 0xFFF0) | c->pk);
}

static void
set_pc20(cpu16 *c, uint32_t v)
{
	v = A20(v) & ~1u;
	c->pk = (uint8_t)(v >> 16);
	c->pc = (uint16_t)v;
}

/* exception entry: stack ret (a 20-bit PK:PC value) and the CCR with its
 * PK, clear PK, fetch the vector from bank 0 */
static void
exception(cpu16 *c, int vec, uint32_t ret)
{
	ret = A20(ret);
	push16(c, (uint16_t)ret);
	push16(c, (uint16_t)((c->ccr & 0xFFF0) | (ret >> 16 & 0xF)));
	c->pk = 0;
	c->pc = rd16(c, (uint32_t)vec * 2) & 0xFFFE;
	c->last_exc = vec;
}

/* ---------------------------------------------------------------- flags */

/* f is CCR[15:8] as computed: start from the current flags and change
 * what the operation defines; the opcode's masks pick what is stored */
static unsigned
fl(cpu16 *c)
{
	return c->ccr >> 8;
}

#define F_S  0x80
#define F_MV 0x40
#define F_H  0x20
#define F_EV 0x10
#define F_N  0x08
#define F_Z  0x04
#define F_V  0x02
#define F_C  0x01

static unsigned
nz8(unsigned f, uint8_t v)
{
	f &= ~(unsigned)(F_N | F_Z);
	if (!v) f |= F_Z;
	if (v & 0x80) f |= F_N;
	return f;
}

static unsigned
nz16(unsigned f, uint16_t v)
{
	f &= ~(unsigned)(F_N | F_Z);
	if (!v) f |= F_Z;
	if (v & 0x8000) f |= F_N;
	return f;
}

static unsigned
setb(unsigned f, unsigned bit, int on)
{
	return on ? f | bit : f & ~bit;
}

static void
apply(cpu16 *c, const struct cpu16_op *o, unsigned f)
{
	unsigned hi = c->ccr >> 8;
	hi = (hi & ~(unsigned)(o->fmask | o->fset | o->fclr)) | (f & o->fmask) | o->fset;
	c->ccr = (uint16_t)(hi << 8 | (c->ccr & 0x00F0));
}

static uint8_t
add8(unsigned *f, uint8_t a, uint8_t b, int cin)
{
	unsigned r = (unsigned)a + b + (unsigned)cin;
	uint8_t v = (uint8_t)r;
	*f = nz8(*f, v);
	*f = setb(*f, F_C, r > 0xFF);
	*f = setb(*f, F_V, ((a ^ v) & (b ^ v) & 0x80) != 0);
	*f = setb(*f, F_H, (((a & 0xF) + (b & 0xF) + (unsigned)cin) & 0x10) != 0);
	return v;
}

static uint8_t
sub8(unsigned *f, uint8_t a, uint8_t b, int cin)
{
	unsigned r = (unsigned)a - b - (unsigned)cin;
	uint8_t v = (uint8_t)r;
	*f = nz8(*f, v);
	*f = setb(*f, F_C, r > 0xFF);
	*f = setb(*f, F_V, ((a ^ b) & (a ^ v) & 0x80) != 0);
	return v;
}

static uint16_t
add16(unsigned *f, uint16_t a, uint16_t b, int cin)
{
	uint32_t r = (uint32_t)a + b + (uint32_t)cin;
	uint16_t v = (uint16_t)r;
	*f = nz16(*f, v);
	*f = setb(*f, F_C, r > 0xFFFF);
	*f = setb(*f, F_V, ((a ^ v) & (b ^ v) & 0x8000) != 0);
	return v;
}

static uint16_t
sub16(unsigned *f, uint16_t a, uint16_t b, int cin)
{
	uint32_t r = (uint32_t)a - b - (uint32_t)cin;
	uint16_t v = (uint16_t)r;
	*f = nz16(*f, v);
	*f = setb(*f, F_C, r > 0xFFFF);
	*f = setb(*f, F_V, ((a ^ b) & (a ^ v) & 0x8000) != 0);
	return v;
}

/* shifts and the like on a w-bit value (8 or 16): kind 0 ASL, 1 ASR,
 * 2 LSR, 3 ROL, 4 ROR, 5 COM, 6 NEG, 7 INC, 8 DEC, 9 CLR, 10 TST */
static uint16_t
unop(unsigned *f, int w, int kind, uint16_t v)
{
	uint16_t msb = w == 8 ? 0x80 : 0x8000, mask = w == 8 ? 0xFF : 0xFFFF;
	uint16_t r = v;
	int cin = (*f & F_C) != 0;
	switch (kind) {
	case 0: *f = setb(*f, F_C, v & msb); r = (uint16_t)(v << 1); break;
	case 1: *f = setb(*f, F_C, v & 1); r = (uint16_t)((v >> 1) | (v & msb)); break;
	case 2: *f = setb(*f, F_C, v & 1); r = (uint16_t)(v >> 1); break;
	case 3: *f = setb(*f, F_C, v & msb); r = (uint16_t)((v << 1) | cin); break;
	case 4: *f = setb(*f, F_C, v & 1); r = (uint16_t)((v >> 1) | (cin ? msb : 0)); break;
	case 5: r = (uint16_t)~v; *f |= F_C; *f &= ~(unsigned)F_V; break;
	case 6: r = (uint16_t)(0 - v); *f = setb(*f, F_C, (r & mask) != 0);
		*f = setb(*f, F_V, (r & mask) == msb); break;
	case 7: r = (uint16_t)(v + 1); *f = setb(*f, F_V, (r & mask) == msb); break;
	case 8: r = (uint16_t)(v - 1); *f = setb(*f, F_V, (r & mask) == (uint16_t)(msb - 1)); break;
	case 9: r = 0; *f &= ~(unsigned)(F_V | F_C); break;
	case 10: *f &= ~(unsigned)(F_V | F_C); break;
	}
	r &= mask;
	*f = w == 8 ? nz8(*f, (uint8_t)r) : nz16(*f, r);
	if (kind <= 4)		/* shifts: V = N xor C */
		*f = setb(*f, F_V, !!(*f & F_N) != !!(*f & F_C));
	return r;
}

/* ---------------------------------------------------------------- MAC */

/* EV: AM[35:31] not all equal; MV: sign overflow of the 36-bit sum
 * (latching SL, the sign right after overflow) */
static void
am_add(cpu16 *c, unsigned *f, int64_t v)
{
	int64_t a = c->am, r = sx36(a + v);
	if (((a ^ r) & (v ^ r)) < 0) {
		if (!(*f & F_MV))
			c->sl = (uint8_t)(r >= 0);	/* complement of AM35 */
		*f |= F_MV;
	}
	c->am = r;
	*f = setb(*f, F_EV, r >= (1LL << 31) || r < -(1LL << 31));
}

static uint16_t
qualify(uint16_t r, uint8_t mask, int off)
{
	if (!mask)
		return (uint16_t)(r + off);
	return (uint16_t)((r & ~mask) | ((r + off) & mask));
}

static uint16_t
am_sat(cpu16 *c)
{
	if (c->ccr & C16_MV)
		return c->sl ? 0x7FFF : 0x8000;
	return c->am < 0 ? 0x8000 : 0x7FFF;
}

static uint32_t idx20(cpu16 *c, int n) { return (uint32_t)c->k[n] << 16 | c->r[n]; }

static void
mac_step(cpu16 *c, unsigned *f, uint8_t xoyo, int rmac)
{
	int32_t p;
	int xo = (int8_t)(xoyo & 0xF0) >> 4, yo = (int8_t)(xoyo << 4) >> 4;
	if (c->hr == 0x8000 && c->ir == 0x8000) {
		p = INT32_MIN;			/* -1 * -1: E:D = $80000000 ... */
		am_add(c, f, 1LL << 31);	/* ... but +1.0 accumulated */
		if (!rmac)
			*f |= F_V;
	} else {
		p = (int32_t)((int16_t)c->hr * (int16_t)c->ir) * 2;
		am_add(c, f, p);
		if (!rmac)
			*f &= ~(unsigned)F_V;
	}
	if (!rmac) {
		c->e = (uint16_t)((uint32_t)p >> 16);
		c->d = (uint16_t)p;
	} else
		c->d = (uint16_t)p;		/* temporary storage */
	c->r[0] = qualify(c->r[0], c->xmsk, xo);
	c->r[1] = qualify(c->r[1], c->ymsk, yo);
	if (!rmac)
		c->r[2] = c->hr;
	c->hr = rd16(c, idx20(c, 0));
	c->ir = rd16(c, idx20(c, 1));
}

/* ---------------------------------------------------------------- operands */

static uint16_t w16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* effective address of an indexed/extended operand at p */
static uint32_t
ea(cpu16 *c, const struct cpu16_op *o, const uint8_t *p)
{
	switch (o->mode) {
	case M_IND8:  return A20(idx20(c, o->reg) + p[0]);
	case M_IND16: return A20(idx20(c, o->reg) + (uint32_t)sx16(w16(p)));
	case M_EIDX:  return A20(idx20(c, o->reg) + (uint32_t)sx16(c->e));
	case M_EXT:   return (uint32_t)c->ek << 16 | w16(p);
	case M_IND20: return A20(idx20(c, o->reg) + (uint32_t)sx20((uint32_t)(p[0] & 0xF) << 16 | w16(p + 1)));
	case M_EXT20: return (uint32_t)(p[0] & 0xF) << 16 | w16(p + 1);
	}
	return 0;
}

static uint8_t
src8(cpu16 *c, const struct cpu16_op *o, const uint8_t *p)
{
	if (o->mode == M_IMM8)
		return p[0];
	return rd8(c, ea(c, o, p));
}

static uint16_t
src16(cpu16 *c, const struct cpu16_op *o, const uint8_t *p)
{
	if (o->mode == M_IMM16)
		return w16(p);
	if (o->mode == M_IMM8)		/* ADDD/ADDE/AIx #8: sign-extended */
		return (uint16_t)sx8(p[0]);
	return rd16(c, ea(c, o, p));
}

/* ---------------------------------------------------------------- branches */

static int
cond(cpu16 *c, int cc)
{
	unsigned f = fl(c);
	int C = !!(f & F_C), Z = !!(f & F_Z), N = !!(f & F_N), V = !!(f & F_V);
	switch (cc) {
	case 0x0: return 1;
	case 0x1: return 0;
	case 0x2: return !(C | Z);
	case 0x3: return C | Z;
	case 0x4: return !C;
	case 0x5: return C;
	case 0x6: return !Z;
	case 0x7: return Z;
	case 0x8: return !V;
	case 0x9: return V;
	case 0xA: return !N;
	case 0xB: return N;
	case 0xC: return !(N ^ V);
	case 0xD: return N ^ V;
	case 0xE: return !(Z | (N ^ V));
	case 0xF: return Z | (N ^ V);
	case 0x10: return !!(f & F_MV);	/* LBMV */
	case 0x11: return !!(f & F_EV);	/* LBEV */
	}
	return 0;
}

/* taken change of flow to op_addr + 6 + off: the pipeline refill is up to
 * three program words (Table 8-2), the instruction's own words are already
 * charged */
static void
jump(cpu16 *c, uint32_t target, int len)
{
	set_pc20(c, target);
	for (int i = len / 2; i < 3; i++)
		c->st += extra(c, cpu16_pc20(c), 2);
}

static void
branch(cpu16 *c, int off, int len)
{
	jump(c, A20(c->op_addr + 6 + (uint32_t)off), len);
}

/* ---------------------------------------------------------------- execute */

static int
illegal(cpu16 *c)
{
	c->illegal = 1;
	exception(c, C16_VEC_ILLEGAL, c->op_addr + 8);
	return 20;
}

static uint16_t kreg(cpu16 *c)
{
	return (uint16_t)(c->ek << 12 | c->k[0] << 8 | c->k[1] << 4 | c->k[2]);
}

static void set_kreg(cpu16 *c, uint16_t v)
{
	c->ek = v >> 12 & 0xF; c->k[0] = v >> 8 & 0xF; c->k[1] = v >> 4 & 0xF; c->k[2] = v & 0xF;
}

static void
add_idx(cpu16 *c, int n, int32_t v)
{
	uint32_t a = A20(idx20(c, n) + (uint32_t)v);
	c->k[n] = (uint8_t)(a >> 16);
	c->r[n] = (uint16_t)a;
}

/* returns the instruction's table clocks; c->st collects access extras */
static int
exec(cpu16 *c)
{
	uint8_t ib[8];
	const struct cpu16_op *o;
	const uint8_t *p;
	int len, cyc;
	unsigned f;
	uint32_t a;
	uint16_t v, w;
	uint8_t b;

	c->op_addr = cpu16_pc20(c);
	v = rd16(c, c->op_addr);
	ib[0] = (uint8_t)(v >> 8);
	ib[1] = (uint8_t)v;
	switch (ib[0]) {
	case 0x17: o = &cpu16_ops[1][ib[1]]; p = ib + 2; break;
	case 0x27: o = &cpu16_ops[2][ib[1]]; p = ib + 2; break;
	case 0x37: o = &cpu16_ops[3][ib[1]]; p = ib + 2; break;
	default:   o = &cpu16_ops[0][ib[0]]; p = ib + 1; break;
	}
	if (o->mn == MN_ILL) {
		set_pc20(c, c->op_addr + 2);
		return illegal(c);
	}
	len = o->len;
	for (int i = 2; i < len; i += 2) {
		v = rd16(c, c->op_addr + (uint32_t)i);
		ib[i] = (uint8_t)(v >> 8);
		ib[i + 1] = (uint8_t)v;
	}
	set_pc20(c, c->op_addr + (uint32_t)len);
	cyc = o->cyc;
	f = fl(c);

	switch (o->mn) {

	/* ---- loads and stores */
	case MN_LDAA: b = src8(c, o, p); sa(c, b); f = nz8(f, b); break;
	case MN_LDAB: b = src8(c, o, p); sb(c, b); f = nz8(f, b); break;
	case MN_LDD:  c->d = src16(c, o, p); f = nz16(f, c->d); break;
	case MN_LDE:  c->e = src16(c, o, p); f = nz16(f, c->e); break;
	case MN_LDX:  c->r[0] = src16(c, o, p); f = nz16(f, c->r[0]); break;
	case MN_LDY:  c->r[1] = src16(c, o, p); f = nz16(f, c->r[1]); break;
	case MN_LDZ:  c->r[2] = src16(c, o, p); f = nz16(f, c->r[2]); break;
	case MN_LDS:  c->sp = src16(c, o, p); f = nz16(f, c->sp); break;
	case MN_LDED: a = ea(c, o, p); c->e = rd16(c, a); c->d = rd16(c, a + 2); break;
	case MN_STAA: wr8(c, ea(c, o, p), ga(c)); f = nz8(f, ga(c)); break;
	case MN_STAB: wr8(c, ea(c, o, p), gb(c)); f = nz8(f, gb(c)); break;
	case MN_STD:  wr16(c, ea(c, o, p), c->d); f = nz16(f, c->d); break;
	case MN_STE:  wr16(c, ea(c, o, p), c->e); f = nz16(f, c->e); break;
	case MN_STX:  wr16(c, ea(c, o, p), c->r[0]); f = nz16(f, c->r[0]); break;
	case MN_STY:  wr16(c, ea(c, o, p), c->r[1]); f = nz16(f, c->r[1]); break;
	case MN_STZ:  wr16(c, ea(c, o, p), c->r[2]); f = nz16(f, c->r[2]); break;
	case MN_STS:  wr16(c, ea(c, o, p), c->sp); f = nz16(f, c->sp); break;
	case MN_STED: a = ea(c, o, p); wr16(c, a, c->e); wr16(c, a + 2, c->d); break;

	/* ---- 8-bit arithmetic and logic */
	case MN_ADDA: sa(c, add8(&f, ga(c), src8(c, o, p), 0)); break;
	case MN_ADDB: sb(c, add8(&f, gb(c), src8(c, o, p), 0)); break;
	case MN_ADCA: sa(c, add8(&f, ga(c), src8(c, o, p), !!(f & F_C))); break;
	case MN_ADCB: sb(c, add8(&f, gb(c), src8(c, o, p), !!(f & F_C))); break;
	case MN_SUBA: sa(c, sub8(&f, ga(c), src8(c, o, p), 0)); break;
	case MN_SUBB: sb(c, sub8(&f, gb(c), src8(c, o, p), 0)); break;
	case MN_SBCA: sa(c, sub8(&f, ga(c), src8(c, o, p), !!(f & F_C))); break;
	case MN_SBCB: sb(c, sub8(&f, gb(c), src8(c, o, p), !!(f & F_C))); break;
	case MN_CMPA: sub8(&f, ga(c), src8(c, o, p), 0); break;
	case MN_CMPB: sub8(&f, gb(c), src8(c, o, p), 0); break;
	case MN_ANDA: b = ga(c) & src8(c, o, p); sa(c, b); f = nz8(f, b); break;
	case MN_ANDB: b = gb(c) & src8(c, o, p); sb(c, b); f = nz8(f, b); break;
	case MN_ORAA: b = ga(c) | src8(c, o, p); sa(c, b); f = nz8(f, b); break;
	case MN_ORAB: b = gb(c) | src8(c, o, p); sb(c, b); f = nz8(f, b); break;
	case MN_EORA: b = ga(c) ^ src8(c, o, p); sa(c, b); f = nz8(f, b); break;
	case MN_EORB: b = gb(c) ^ src8(c, o, p); sb(c, b); f = nz8(f, b); break;
	case MN_BITA: f = nz8(f, ga(c) & src8(c, o, p)); break;
	case MN_BITB: f = nz8(f, gb(c) & src8(c, o, p)); break;
	case MN_ABA:  sa(c, add8(&f, ga(c), gb(c), 0)); break;
	case MN_SBA:  sa(c, sub8(&f, ga(c), gb(c), 0)); break;
	case MN_CBA:  sub8(&f, ga(c), gb(c), 0); break;

	/* ---- 16-bit arithmetic and logic */
	case MN_ADDD: c->d = add16(&f, c->d, src16(c, o, p), 0); break;
	case MN_ADDE: c->e = add16(&f, c->e, src16(c, o, p), 0); break;
	case MN_ADCD: c->d = add16(&f, c->d, src16(c, o, p), !!(f & F_C)); break;
	case MN_ADCE: c->e = add16(&f, c->e, src16(c, o, p), !!(f & F_C)); break;
	case MN_SUBD: c->d = sub16(&f, c->d, src16(c, o, p), 0); break;
	case MN_SUBE: c->e = sub16(&f, c->e, src16(c, o, p), 0); break;
	case MN_SBCD: c->d = sub16(&f, c->d, src16(c, o, p), !!(f & F_C)); break;
	case MN_SBCE: c->e = sub16(&f, c->e, src16(c, o, p), !!(f & F_C)); break;
	case MN_CPD:  sub16(&f, c->d, src16(c, o, p), 0); break;
	case MN_CPE:  sub16(&f, c->e, src16(c, o, p), 0); break;
	case MN_CPX:  sub16(&f, c->r[0], src16(c, o, p), 0); break;
	case MN_CPY:  sub16(&f, c->r[1], src16(c, o, p), 0); break;
	case MN_CPZ:  sub16(&f, c->r[2], src16(c, o, p), 0); break;
	case MN_CPS:  sub16(&f, c->sp, src16(c, o, p), 0); break;
	case MN_ANDD: c->d &= src16(c, o, p); f = nz16(f, c->d); break;
	case MN_ANDE: c->e &= src16(c, o, p); f = nz16(f, c->e); break;
	case MN_ORD:  c->d |= src16(c, o, p); f = nz16(f, c->d); break;
	case MN_ORE:  c->e |= src16(c, o, p); f = nz16(f, c->e); break;
	case MN_EORD: c->d ^= src16(c, o, p); f = nz16(f, c->d); break;
	case MN_EORE: c->e ^= src16(c, o, p); f = nz16(f, c->e); break;
	case MN_ADE:  c->e = add16(&f, c->e, c->d, 0); break;
	case MN_SDE:  c->e = sub16(&f, c->e, c->d, 0); break;

	/* ---- read-modify-write: memory byte, word, accumulators */
#define RMW8(kind)  a = ea(c, o, p); wr8(c, a, (uint8_t)unop(&f, 8, kind, rd8(c, a)))
#define RMW16(kind) a = ea(c, o, p); wr16(c, a, unop(&f, 16, kind, rd16(c, a)))
	case MN_ASL:  RMW8(0); break;
	case MN_ASR:  RMW8(1); break;
	case MN_LSR:  RMW8(2); break;
	case MN_ROL:  RMW8(3); break;
	case MN_ROR:  RMW8(4); break;
	case MN_COM:  RMW8(5); break;
	case MN_NEG:  RMW8(6); break;
	case MN_INC:  RMW8(7); break;
	case MN_DEC:  RMW8(8); break;
	case MN_CLR:  a = ea(c, o, p); wr8(c, a, (uint8_t)unop(&f, 8, 9, 0)); break;
	case MN_TST:  unop(&f, 8, 10, rd8(c, ea(c, o, p))); break;
	case MN_ASLW: RMW16(0); break;
	case MN_ASRW: RMW16(1); break;
	case MN_LSRW: RMW16(2); break;
	case MN_ROLW: RMW16(3); break;
	case MN_RORW: RMW16(4); break;
	case MN_COMW: RMW16(5); break;
	case MN_NEGW: RMW16(6); break;
	case MN_INCW: RMW16(7); break;
	case MN_DECW: RMW16(8); break;
	case MN_CLRW: a = ea(c, o, p); wr16(c, a, unop(&f, 16, 9, 0)); break;
	case MN_TSTW: unop(&f, 16, 10, rd16(c, ea(c, o, p))); break;
#define ACC8A(kind) sa(c, (uint8_t)unop(&f, 8, kind, ga(c)))
#define ACC8B(kind) sb(c, (uint8_t)unop(&f, 8, kind, gb(c)))
	case MN_ASLA: ACC8A(0); break;
	case MN_ASRA: ACC8A(1); break;
	case MN_LSRA: ACC8A(2); break;
	case MN_ROLA: ACC8A(3); break;
	case MN_RORA: ACC8A(4); break;
	case MN_COMA: ACC8A(5); break;
	case MN_NEGA: ACC8A(6); break;
	case MN_INCA: ACC8A(7); break;
	case MN_DECA: ACC8A(8); break;
	case MN_CLRA: ACC8A(9); break;
	case MN_TSTA: ACC8A(10); break;
	case MN_ASLB: ACC8B(0); break;
	case MN_ASRB: ACC8B(1); break;
	case MN_LSRB: ACC8B(2); break;
	case MN_ROLB: ACC8B(3); break;
	case MN_RORB: ACC8B(4); break;
	case MN_COMB: ACC8B(5); break;
	case MN_NEGB: ACC8B(6); break;
	case MN_INCB: ACC8B(7); break;
	case MN_DECB: ACC8B(8); break;
	case MN_CLRB: ACC8B(9); break;
	case MN_TSTB: ACC8B(10); break;
	case MN_ASLD: c->d = unop(&f, 16, 0, c->d); break;
	case MN_ASRD: c->d = unop(&f, 16, 1, c->d); break;
	case MN_LSRD: c->d = unop(&f, 16, 2, c->d); break;
	case MN_ROLD: c->d = unop(&f, 16, 3, c->d); break;
	case MN_RORD: c->d = unop(&f, 16, 4, c->d); break;
	case MN_COMD: c->d = unop(&f, 16, 5, c->d); break;
	case MN_NEGD: c->d = unop(&f, 16, 6, c->d); break;
	case MN_CLRD: c->d = unop(&f, 16, 9, c->d); break;
	case MN_TSTD: unop(&f, 16, 10, c->d); break;
	case MN_ASLE: c->e = unop(&f, 16, 0, c->e); break;
	case MN_ASRE: c->e = unop(&f, 16, 1, c->e); break;
	case MN_LSRE: c->e = unop(&f, 16, 2, c->e); break;
	case MN_ROLE: c->e = unop(&f, 16, 3, c->e); break;
	case MN_RORE: c->e = unop(&f, 16, 4, c->e); break;
	case MN_COME: c->e = unop(&f, 16, 5, c->e); break;
	case MN_NEGE: c->e = unop(&f, 16, 6, c->e); break;
	case MN_CLRE: c->e = unop(&f, 16, 9, c->e); break;
	case MN_TSTE: unop(&f, 16, 10, c->e); break;

	/* ---- bit manipulation: mask first, then the address operand */
	case MN_BSET: a = ea(c, o, p + 1); b = rd8(c, a) | p[0]; wr8(c, a, b); f = nz8(f, b); break;
	case MN_BCLR: a = ea(c, o, p + 1); b = rd8(c, a) & (uint8_t)~p[0]; wr8(c, a, b); f = nz8(f, b); break;
	case MN_BSETW: a = ea(c, o, p); w = rd16(c, a) | w16(p + 2); wr16(c, a, w); f = nz16(f, w); break;
	case MN_BCLRW: a = ea(c, o, p); w = rd16(c, a) & (uint16_t)~w16(p + 2); wr16(c, a, w); f = nz16(f, w); break;
	case MN_BRSET:
	case MN_BRCLR: {
		int off;
		b = rd8(c, ea(c, o, p + 1));
		if (o->mn == MN_BRSET)
			b = (uint8_t)~b;
		off = o->mode == M_IND8 ? sx8(p[2]) : sx16(w16(p + 3));
		if (!(b & p[0])) {
			cyc = o->alt > o->cyc ? o->alt : o->cyc;
			branch(c, off, len);
		} else
			cyc = o->alt < o->cyc ? o->alt : o->cyc;
		break;
	}

	/* ---- moves: MOVB/MOVW ff hhll (IXP), hhll hhll (EXT to EXT) */
	case MN_MOVB:
	case MN_MOVW: {
		int word = o->mn == MN_MOVW;
		uint32_t s, dst;
		if (o->mode == M_EXT_EXT) {
			s = (uint32_t)c->ek << 16 | w16(p);
			dst = (uint32_t)c->ek << 16 | w16(p + 2);
		} else if (o->mode == M_IXP_EXT) {
			s = idx20(c, 0);
			dst = (uint32_t)c->ek << 16 | w16(p + 1);
		} else {
			s = (uint32_t)c->ek << 16 | w16(p + 1);
			dst = idx20(c, 0);
		}
		if (word) {
			w = rd16(c, s);
			wr16(c, dst, w);
			f = nz16(f, w);
		} else {
			b = rd8(c, s);
			wr8(c, dst, b);
			f = nz8(f, b);
		}
		if (o->mode != M_EXT_EXT)
			add_idx(c, 0, sx8(p[0]));
		break;
	}

	/* ---- transfers and exchanges */
	case MN_TAB:  sb(c, ga(c)); f = nz8(f, gb(c)); break;
	case MN_TBA:  sa(c, gb(c)); f = nz8(f, ga(c)); break;
	case MN_TDE:  c->e = c->d; f = nz16(f, c->e); break;
	case MN_TED:  c->d = c->e; f = nz16(f, c->d); break;
	case MN_XGAB: c->d = (uint16_t)(c->d << 8 | c->d >> 8); break;
	case MN_XGDE: w = c->d; c->d = c->e; c->e = w; break;
	case MN_XGDX: w = c->d; c->d = c->r[0]; c->r[0] = w; break;
	case MN_XGDY: w = c->d; c->d = c->r[1]; c->r[1] = w; break;
	case MN_XGDZ: w = c->d; c->d = c->r[2]; c->r[2] = w; break;
	case MN_XGEX: w = c->e; c->e = c->r[0]; c->r[0] = w; break;
	case MN_XGEY: w = c->e; c->e = c->r[1]; c->r[1] = w; break;
	case MN_XGEZ: w = c->e; c->e = c->r[2]; c->r[2] = w; break;
	case MN_SXT:  sa(c, (gb(c) & 0x80) ? 0xFF : 0x00); f = nz8(f, ga(c)); break;
	case MN_TPA:  sa(c, (uint8_t)(c->ccr >> 8)); break;
	case MN_TPD:  c->d = cpu16_ccr(c); break;
	case MN_TAP:
		c->ccr = (uint16_t)(ga(c) << 8 | (c->ccr & 0x00F0));
		c->irq_inhibit = 1;
		return cyc;
	case MN_TDP:
		c->ccr = c->d & 0xFFF0;
		c->irq_inhibit = 1;
		return cyc;
	case MN_ANDP:
		c->ccr &= w16(p) | 0x000F;
		c->irq_inhibit = 1;
		return cyc;
	case MN_ORP:
		c->ccr |= w16(p) & 0xFFF0;
		c->irq_inhibit = 1;
		return cyc;

	/* ---- index registers and extension fields */
	case MN_ABX: case MN_ABY: case MN_ABZ:
		add_idx(c, o->mn - MN_ABX, gb(c));
		break;
	case MN_ADX: case MN_ADY: case MN_ADZ:
		add_idx(c, o->mn - MN_ADX, sx16(c->d));
		break;
	case MN_AEX: case MN_AEY: case MN_AEZ:
		add_idx(c, o->mn - MN_AEX, sx16(c->e));
		break;
	case MN_AIX: case MN_AIY: case MN_AIZ: {
		int n = o->mn - MN_AIX;
		add_idx(c, n, sx16(src16(c, o, p)));
		f = setb(f, F_Z, c->r[n] == 0);
		break;
	}
	case MN_AIS: sp_add(c, sx16(src16(c, o, p))); break;
	case MN_TSX: case MN_TSY: case MN_TSZ: {
		int n = o->mn - MN_TSX;
		a = A20(sp20(c) + 2);
		c->k[n] = (uint8_t)(a >> 16);
		c->r[n] = (uint16_t)a;
		break;
	}
	case MN_TXS: case MN_TYS: case MN_TZS:
		a = A20(idx20(c, o->mn == MN_TXS ? 0 : o->mn == MN_TYS ? 1 : 2) - 2);
		c->sk = (uint8_t)(a >> 16);
		c->sp = (uint16_t)a;
		break;
	case MN_TXY: c->r[1] = c->r[0]; c->k[1] = c->k[0]; break;
	case MN_TXZ: c->r[2] = c->r[0]; c->k[2] = c->k[0]; break;
	case MN_TYX: c->r[0] = c->r[1]; c->k[0] = c->k[1]; break;
	case MN_TYZ: c->r[2] = c->r[1]; c->k[2] = c->k[1]; break;
	case MN_TZX: c->r[0] = c->r[2]; c->k[0] = c->k[2]; break;
	case MN_TZY: c->r[1] = c->r[2]; c->k[1] = c->k[2]; break;
	case MN_TBEK: c->ek = gb(c) & 0xF; break;
	case MN_TBXK: c->k[0] = gb(c) & 0xF; break;
	case MN_TBYK: c->k[1] = gb(c) & 0xF; break;
	case MN_TBZK: c->k[2] = gb(c) & 0xF; break;
	case MN_TBSK: c->sk = gb(c) & 0xF; break;
	case MN_TEKB: sb(c, c->ek); break;
	case MN_TXKB: sb(c, c->k[0]); break;
	case MN_TYKB: sb(c, c->k[1]); break;
	case MN_TZKB: sb(c, c->k[2]); break;
	case MN_TSKB: sb(c, c->sk); break;

	/* ---- multiply and divide */
	case MN_MUL:
		c->d = (uint16_t)(ga(c) * gb(c));
		f = setb(f, F_C, c->d & 0x80);
		break;
	case MN_EMUL: {
		uint32_t r = (uint32_t)c->e * c->d;
		c->e = (uint16_t)(r >> 16); c->d = (uint16_t)r;
		f = setb(f, F_N, c->e & 0x8000); f = setb(f, F_Z, r == 0); f = setb(f, F_C, c->d & 0x8000);
		break;
	}
	case MN_EMULS: {
		uint32_t r = (uint32_t)((int32_t)(int16_t)c->e * (int16_t)c->d);
		c->e = (uint16_t)(r >> 16); c->d = (uint16_t)r;
		f = setb(f, F_N, c->e & 0x8000); f = setb(f, F_Z, r == 0); f = setb(f, F_C, c->d & 0x8000);
		break;
	}
	case MN_FMULS: {
		uint32_t r;
		if (c->e == 0x8000 && c->d == 0x8000) {
			r = 0x80000000u;
			f |= F_V;
		} else {
			r = (uint32_t)((int32_t)(int16_t)c->e * (int16_t)c->d) << 1;
			f &= ~(unsigned)F_V;
		}
		c->e = (uint16_t)(r >> 16); c->d = (uint16_t)r;
		f = setb(f, F_N, c->e & 0x8000); f = setb(f, F_Z, r == 0); f = setb(f, F_C, c->d & 0x8000);
		break;
	}
	case MN_IDIV:
		f = setb(f, F_C, c->r[0] == 0);
		if (c->r[0] == 0)
			c->r[0] = 0xFFFF;
		else {
			uint16_t q = c->d / c->r[0];
			c->d = c->d % c->r[0];
			c->r[0] = q;
		}
		f = setb(f, F_Z, c->r[0] == 0);
		break;
	case MN_FDIV:
		f = setb(f, F_C, c->r[0] == 0);
		f = setb(f, F_V, c->r[0] <= c->d);
		if (c->r[0] <= c->d)
			c->r[0] = 0xFFFF;
		else {
			uint32_t n = (uint32_t)c->d << 16;
			uint16_t q = (uint16_t)(n / c->r[0]);
			c->d = (uint16_t)(n % c->r[0]);
			c->r[0] = q;
		}
		f = setb(f, F_Z, c->r[0] == 0);
		break;
	case MN_EDIV:
	case MN_EDIVS: {
		uint16_t q, r;
		int ovf;
		if (c->r[0] == 0) {
			apply(c, o, f);
			exception(c, C16_VEC_ZERODIV, c->op_addr + 8);
			return cyc;
		}
		if (o->mn == MN_EDIV) {
			uint32_t n = (uint32_t)c->e << 16 | c->d, dv = c->r[0], qq = n / dv;
			r = (uint16_t)(n % dv);
			q = (uint16_t)qq;
			ovf = qq > 0xFFFF;
			f = setb(f, F_C, 2u * r >= dv);
		} else {
			int64_t n = (int32_t)((uint32_t)c->e << 16 | c->d), dv = (int16_t)c->r[0];
			int64_t qq = n / dv;
			r = (uint16_t)(int16_t)(n % dv);
			q = (uint16_t)qq;
			ovf = qq > 0x7FFF || qq < -0x8000;
			f = setb(f, F_C, 2 * (llabs((int64_t)(int16_t)r)) >= llabs(dv));
		}
		c->r[0] = q;
		c->d = r;
		f = nz16(f, q);
		f = setb(f, F_V, ovf);
		break;
	}

	/* ---- MAC unit */
	case MN_CLRM: c->am = 0; f &= ~(unsigned)(F_EV | F_MV); break;
	case MN_TEDM: c->am = sx16(c->e) * 65536LL + c->d; f &= ~(unsigned)(F_EV | F_MV); break;
	case MN_TEM:  c->am = sx16(c->e) * 65536LL; f &= ~(unsigned)(F_EV | F_MV); break;
	case MN_ACE:  am_add(c, &f, sx16(c->e) * 65536LL); break;
	case MN_ACED: am_add(c, &f, (int32_t)((uint32_t)c->e << 16 | c->d)); break;
	case MN_ASLM: {
		int64_t old = c->am;
		f = setb(f, F_C, old < 0);
		c->am = sx36(old * 2);
		if ((old < 0) != (c->am < 0))
			f |= F_MV;
		f = setb(f, F_EV, c->am >= (1LL << 31) || c->am < -(1LL << 31));
		f = setb(f, F_N, c->am < 0);
		break;
	}
	case MN_ASRM:
		f = setb(f, F_C, c->am & 1);
		c->am >>= 1;
		f = setb(f, F_EV, c->am >= (1LL << 31) || c->am < -(1LL << 31));
		f = setb(f, F_N, c->am < 0);
		break;
	case MN_TMER: {
		int64_t t = c->am, lo = t & 0xFFFF;
		if (lo > 0x8000 || (lo == 0x8000 && (t & 0x10000)))
			t += 0x10000;
		t = sx36(t & ~0xFFFFLL);
		if (t >= (1LL << 31) || t < -(1LL << 31))
			f |= F_EV;
		if ((c->am < 0) != (t < 0) && c->am >= 0)
			f |= F_MV;
		if ((c->ccr & C16_SM) && (f & (F_EV | F_MV)))
			c->e = am_sat(c);
		else
			c->e = (uint16_t)(t >> 16);
		f = nz16(f, c->e);
		break;
	}
	case MN_TMET:
		if ((c->ccr & C16_SM) && (f & (F_EV | F_MV)))
			c->e = am_sat(c);
		else
			c->e = (uint16_t)(c->am >> 16);
		f = nz16(f, c->e);
		break;
	case MN_TMXED:
		c->r[0] = (uint16_t)((c->am >> 32) & 0xF) | (c->am < 0 ? 0xFFF0 : 0);
		c->e = (uint16_t)(c->am >> 16);
		c->d = (uint16_t)c->am;
		break;
	case MN_TDMSK: c->xmsk = ga(c); c->ymsk = gb(c); break;
	case MN_LDHI:
		c->hr = rd16(c, idx20(c, 0));
		c->ir = rd16(c, idx20(c, 1));
		break;
	case MN_MAC:
		mac_step(c, &f, p[0], 0);
		break;
	case MN_RMAC:
		/* one iteration per step, so interrupts come between them
		 * (11.7.3.2); the PC stays on RMAC until E goes negative */
		cyc = c->rmac ? o->alt : o->cyc + o->alt;
		mac_step(c, &f, p[0], 1);
		c->e--;
		c->rmac = !(c->e & 0x8000);
		if (c->rmac)
			set_pc20(c, c->op_addr);
		break;
	case MN_PSHMAC:
		push16(c, c->hr);
		push16(c, c->ir);
		push16(c, (uint16_t)c->am);
		push16(c, (uint16_t)(c->am >> 16));
		push16(c, (uint16_t)((c->sl ? 0x8000 : 0) | ((c->am >> 32) & 0xF)));
		push16(c, (uint16_t)(c->xmsk << 8 | c->ymsk));
		break;
	case MN_PULMAC: {
		uint64_t m;
		w = pull16(c); c->xmsk = (uint8_t)(w >> 8); c->ymsk = (uint8_t)w;
		w = pull16(c); c->sl = !!(w & 0x8000);
		m = (uint64_t)(w & 0xF) << 32;
		m |= (uint64_t)pull16(c) << 16;
		m |= pull16(c);
		c->am = sx36((int64_t)m);
		c->ir = pull16(c);
		c->hr = pull16(c);
		break;
	}

	/* ---- stack */
	case MN_PSHA: sp_add(c, 1); wr8(c, sp20(c), ga(c)); sp_add(c, -2); break;
	case MN_PSHB: sp_add(c, 1); wr8(c, sp20(c), gb(c)); sp_add(c, -2); break;
	case MN_PULA: sp_add(c, 2); sa(c, rd8(c, sp20(c))); sp_add(c, -1); break;
	case MN_PULB: sp_add(c, 2); sb(c, rd8(c, sp20(c))); sp_add(c, -1); break;
	case MN_PSHM: {
		int n = 0;
		b = p[0];
		if (b & 0x01) { push16(c, c->d); n++; }
		if (b & 0x02) { push16(c, c->e); n++; }
		if (b & 0x04) { push16(c, c->r[0]); n++; }
		if (b & 0x08) { push16(c, c->r[1]); n++; }
		if (b & 0x10) { push16(c, c->r[2]); n++; }
		if (b & 0x20) { push16(c, kreg(c)); n++; }
		if (b & 0x40) { push16(c, cpu16_ccr(c)); n++; }
		cyc = 4 + 2 * n;
		break;
	}
	case MN_PULM: {
		int n = 0;
		b = p[0];
		if (b & 0x01) { c->ccr = pull16(c) & 0xFFF0; n++; }
		if (b & 0x02) { set_kreg(c, pull16(c)); n++; }
		if (b & 0x04) { c->r[2] = pull16(c); n++; }
		if (b & 0x08) { c->r[1] = pull16(c); n++; }
		if (b & 0x10) { c->r[0] = pull16(c); n++; }
		if (b & 0x20) { c->e = pull16(c); n++; }
		if (b & 0x40) { c->d = pull16(c); n++; }
		cyc = 4 + 2 * (n + 1);
		return cyc;		/* CCR only as pulled */
	}

	/* ---- program control */
	case MN_BRA: case MN_BRN: case MN_BHI: case MN_BLS: case MN_BCC: case MN_BCS:
	case MN_BNE: case MN_BEQ: case MN_BVC: case MN_BVS: case MN_BPL: case MN_BMI:
	case MN_BGE: case MN_BLT: case MN_BGT: case MN_BLE:
		if (cond(c, ib[0] & 0xF))
			branch(c, sx8(p[0]), len);
		else
			cyc = o->alt;
		break;
	case MN_LBRA: case MN_LBRN: case MN_LBHI: case MN_LBLS: case MN_LBCC: case MN_LBCS:
	case MN_LBNE: case MN_LBEQ: case MN_LBVC: case MN_LBVS: case MN_LBPL: case MN_LBMI:
	case MN_LBGE: case MN_LBLT: case MN_LBGT: case MN_LBLE: case MN_LBMV: case MN_LBEV:
		if (cond(c, ib[1] & 0x1F))
			branch(c, sx16(w16(p)), len);
		else
			cyc = o->alt;
		break;
	case MN_BSR:
		a = A20(c->op_addr + 4);
		push16(c, (uint16_t)a);
		push16(c, (uint16_t)((c->ccr & 0xFFF0) | (a >> 16)));
		branch(c, sx8(p[0]), len);
		break;
	case MN_LBSR:
		a = A20(c->op_addr + 6);
		push16(c, (uint16_t)a);
		push16(c, (uint16_t)((c->ccr & 0xFFF0) | (a >> 16)));
		branch(c, sx16(w16(p)), len);
		break;
	case MN_JMP:
		jump(c, ea(c, o, p), len);
		break;
	case MN_JSR:
		a = ea(c, o, p);
		{
			uint32_t r = A20(c->op_addr + 6);
			push16(c, (uint16_t)r);
			push16(c, (uint16_t)((c->ccr & 0xFFF0) | (r >> 16)));
		}
		jump(c, a, len);
		break;
	case MN_RTS:
		w = pull16(c);			/* CCR: only PK */
		a = (uint32_t)(w & 0xF) << 16 | pull16(c);
		jump(c, a - 2, len);
		break;
	case MN_RTI:
		w = pull16(c);
		c->ccr = w & 0xFFF0;
		a = (uint32_t)(w & 0xF) << 16 | pull16(c);
		jump(c, a - 6, len);
		return cyc;
	case MN_SWI:
		exception(c, C16_VEC_SWI, c->op_addr + 8);
		return cyc;
	case MN_WAI:
		c->waiting = 1;
		break;
	case MN_LPSTOP:
		if (!(c->ccr & C16_S)) {
			c->waiting = c->stopped = 1;
			cyc = o->alt;
		}
		break;
	case MN_BGND:
		return illegal(c);
	case MN_NOP:
		break;
	case MN_DAA: {
		uint8_t av = ga(c), corr = 0;
		int cflag = !!(f & F_C), h = !!(f & F_H);
		uint8_t hi = av >> 4, lo = av & 0xF;
		if (h || lo > 9)
			corr |= 0x06;
		if (cflag || hi > 9 || (hi > 8 && lo > 9))
			corr |= 0x60;
		av = (uint8_t)(av + corr);
		sa(c, av);
		f = nz8(f, av);
		f = setb(f, F_C, cflag || (corr & 0x60));
		break;
	}
	default:
		return illegal(c);
	}
	apply(c, o, f);
	return cyc;
}

/* ---------------------------------------------------------------- api */

void
cpu16_init(cpu16 *c, const cpu16_bus *bus)
{
	memset(c, 0, sizeof(*c));
	c->bus = *bus;
	c->last_exc = -1;
}

void
cpu16_reset(cpu16 *c)
{
	uint16_t w;
	c->st = 0;
	c->ccr = (uint16_t)((c->ccr & ~(C16_IP | C16_SM)) | C16_IP | C16_S);
	c->ek = c->k[0] = c->k[1] = c->k[2] = 0;
	c->waiting = c->stopped = c->rmac = c->irq_inhibit = 0;
	/* reset vectors in program space, bank 0 */
	w = rd16(c, 0);
	c->k[2] = w >> 8 & 0xF;
	c->sk = w >> 4 & 0xF;
	c->pk = w & 0xF;
	c->pc = rd16(c, 2) & 0xFFFE;
	c->sp = rd16(c, 4);
	c->r[2] = rd16(c, 6);
	c->last_exc = C16_VEC_RESET;
	c->clocks += 40 + (uint64_t)c->st;
	c->st = 0;
}

int
cpu16_step(cpu16 *c)
{
	int n, lvl = c->irq_level;
	c->st = 0;
	c->illegal = 0;
	if (!c->irq_inhibit && lvl && (lvl == 7 || lvl > (c->ccr & C16_IP) >> 5)) {
		int vec;
		uint32_t ret = cpu16_pc20(c) + 6;
		c->waiting = c->stopped = 0;
		c->rmac = 0;		/* resumes from its first-iteration count */
		push16(c, (uint16_t)A20(ret));
		push16(c, (uint16_t)((c->ccr & 0xFFF0) | (A20(ret) >> 16)));
		c->ccr = (uint16_t)((c->ccr & ~C16_IP) | (lvl << 5));
		c->pk = 0;
		vec = c->bus.iack(c->bus.ctx, lvl);
		c->pc = rd16(c, (uint32_t)vec * 2) & 0xFFFE;
		c->last_exc = vec;
		jump(c, cpu16_pc20(c), 0);
		/* exception processing: two stack writes, IACK, vector read,
		 * three refill fetches; ~16 internal clocks besides */
		n = 26 + c->st;
		c->clocks += (uint64_t)n;
		return n;
	}
	c->irq_inhibit = 0;
	if (c->waiting) {
		c->clocks += 2;
		return 2;
	}
	n = exec(c) + c->st;
	c->clocks += (uint64_t)n;
	return n;
}

int
cpu16_oplen(cpu16 *c, uint32_t a)
{
	uint8_t b0 = c->bus.rd8(c->bus.ctx, A20(a)), b1 = c->bus.rd8(c->bus.ctx, A20(a + 1));
	const struct cpu16_op *o;
	switch (b0) {
	case 0x17: o = &cpu16_ops[1][b1]; break;
	case 0x27: o = &cpu16_ops[2][b1]; break;
	case 0x37: o = &cpu16_ops[3][b1]; break;
	default:   o = &cpu16_ops[0][b0]; break;
	}
	return o->mn == MN_ILL ? 2 : o->len;
}
