/*
 * Z80 CPU core.  See z80.h.
 *
 * Timing: T-states per instruction follow the Zilog tables. Memory and I/O
 * accesses happen through callbacks without intra-instruction timing; the
 * caller advances peripherals per instruction.
 */
#include "z80.h"

#include <string.h>

/* ---------------------------------------------------------------- tables */

static uint8_t sz53[256];	/* S, Z, Y, X flags of a byte */
static uint8_t sz53p[256];	/* ... plus parity */
static int     tables_done;

static void
init_tables(void)
{
	for (int i = 0; i < 256; i++) {
		uint8_t f = i & (Z80_SF | Z80_YF | Z80_XF);
		if (i == 0)
			f |= Z80_ZF;
		sz53[i] = f;
		int p = 0;
		for (int b = 0; b < 8; b++)
			p ^= (i >> b) & 1;
		sz53p[i] = f | (p ? 0 : Z80_PF);
	}
	tables_done = 1;
}

/* base T-states for unprefixed opcodes; conditional ones are "not taken" */
static const uint8_t cyc_main[256] = {
	 4,10, 7, 6, 4, 4, 7, 4, 4,11, 7, 6, 4, 4, 7, 4,
	 8,10, 7, 6, 4, 4, 7, 4,12,11, 7, 6, 4, 4, 7, 4,
	 7,10,16, 6, 4, 4, 7, 4, 7,11,16, 6, 4, 4, 7, 4,
	 7,10,13, 6,11,11,10, 4, 7,11,13, 6, 4, 4, 7, 4,
	 4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
	 4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
	 4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
	 7, 7, 7, 7, 7, 7, 4, 7, 4, 4, 4, 4, 4, 4, 7, 4,
	 4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
	 4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
	 4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
	 4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
	 5,10,10,10,10,11, 7,11, 5,10,10, 0,10,17, 7,11,
	 5,10,10,11,10,11, 7,11, 5, 4,10,11,10, 0, 7,11,
	 5,10,10,19,10,11, 7,11, 5, 4,10, 4,10, 0, 7,11,
	 5,10,10, 4,10,11, 7,11, 5, 6,10, 4,10, 0, 7,11,
};

/* ---------------------------------------------------------------- helpers */

#define BC  ((uint16_t)((z->b << 8) | z->c))
#define DE  ((uint16_t)((z->d << 8) | z->e))
#define HL  ((uint16_t)((z->h << 8) | z->l))
#define AF  ((uint16_t)((z->a << 8) | z->f))
#define SET_BC(v) do { uint16_t v_ = (v); z->b = v_ >> 8; z->c = v_; } while (0)
#define SET_DE(v) do { uint16_t v_ = (v); z->d = v_ >> 8; z->e = v_; } while (0)
#define SET_HL(v) do { uint16_t v_ = (v); z->h = v_ >> 8; z->l = v_; } while (0)
#define SET_AF(v) do { uint16_t v_ = (v); z->a = v_ >> 8; z->f = v_; } while (0)

static inline uint8_t rd(z80 *z, uint16_t a) { return z->read(z->ctx, a); }
static inline void wr(z80 *z, uint16_t a, uint8_t v) { z->write(z->ctx, a, v); }

static inline uint16_t
rd16(z80 *z, uint16_t a)
{
	uint8_t lo = rd(z, a);
	return lo | (rd(z, a + 1) << 8);
}

static inline void
wr16(z80 *z, uint16_t a, uint16_t v)
{
	wr(z, a, v);
	wr(z, a + 1, v >> 8);
}

static inline uint8_t
fetch_m1(z80 *z)
{
	uint8_t op = rd(z, z->pc++);
	z->r = (z->r & 0x80) | ((z->r + 1) & 0x7f);
	z->m1++;
	return op;
}

static inline uint8_t fetch8(z80 *z) { return rd(z, z->pc++); }

static inline uint16_t
fetch16(z80 *z)
{
	uint16_t v = rd16(z, z->pc);
	z->pc += 2;
	return v;
}

static inline void
push(z80 *z, uint16_t v)
{
	z->sp -= 2;
	wr16(z, z->sp, v);
}

static inline uint16_t
pop(z80 *z)
{
	uint16_t v = rd16(z, z->sp);
	z->sp += 2;
	return v;
}

#define SETF(v) do { z->f = (v); z->q = z->f; } while (0)

/* ---------------------------------------------------------------- ALU */

static inline void
add8(z80 *z, uint8_t v, int carry)
{
	unsigned r = z->a + v + carry;
	uint8_t f = sz53[r & 0xff] | ((r >> 8) & Z80_CF);
	f |= (z->a ^ v ^ r) & Z80_HF;
	f |= (((z->a ^ ~v) & (z->a ^ r)) >> 5) & Z80_PF;
	z->a = r;
	SETF(f);
}

static inline void
sub8(z80 *z, uint8_t v, int carry)
{
	unsigned r = z->a - v - carry;
	uint8_t f = sz53[r & 0xff] | ((r >> 8) & Z80_CF) | Z80_NF;
	f |= (z->a ^ v ^ r) & Z80_HF;
	f |= (((z->a ^ v) & (z->a ^ r)) >> 5) & Z80_PF;
	z->a = r;
	SETF(f);
}

static inline void
cp8(z80 *z, uint8_t v)
{
	unsigned r = z->a - v;
	uint8_t f = (sz53[r & 0xff] & ~(Z80_XF | Z80_YF))
	          | (v & (Z80_XF | Z80_YF))
	          | ((r >> 8) & Z80_CF) | Z80_NF;
	f |= (z->a ^ v ^ r) & Z80_HF;
	f |= (((z->a ^ v) & (z->a ^ r)) >> 5) & Z80_PF;
	SETF(f);
}

static inline void and8(z80 *z, uint8_t v) { z->a &= v; SETF(sz53p[z->a] | Z80_HF); }
static inline void xor8(z80 *z, uint8_t v) { z->a ^= v; SETF(sz53p[z->a]); }
static inline void or8(z80 *z, uint8_t v)  { z->a |= v; SETF(sz53p[z->a]); }

static void
alu(z80 *z, int op, uint8_t v)
{
	switch (op) {
	case 0: add8(z, v, 0); break;
	case 1: add8(z, v, z->f & Z80_CF); break;
	case 2: sub8(z, v, 0); break;
	case 3: sub8(z, v, z->f & Z80_CF); break;
	case 4: and8(z, v); break;
	case 5: xor8(z, v); break;
	case 6: or8(z, v); break;
	case 7: cp8(z, v); break;
	}
}

static inline uint8_t
inc8(z80 *z, uint8_t v)
{
	uint8_t r = v + 1;
	uint8_t f = (z->f & Z80_CF) | sz53[r];
	if ((r & 0x0f) == 0)
		f |= Z80_HF;
	if (r == 0x80)
		f |= Z80_PF;
	SETF(f);
	return r;
}

static inline uint8_t
dec8(z80 *z, uint8_t v)
{
	uint8_t r = v - 1;
	uint8_t f = (z->f & Z80_CF) | sz53[r] | Z80_NF;
	if ((v & 0x0f) == 0)
		f |= Z80_HF;
	if (v == 0x80)
		f |= Z80_PF;
	SETF(f);
	return r;
}

static inline uint16_t
add16(z80 *z, uint16_t a, uint16_t b)
{
	unsigned r = a + b;
	z->wz = a + 1;
	uint8_t f = (z->f & (Z80_SF | Z80_ZF | Z80_PF))
	          | ((r >> 16) & Z80_CF)
	          | ((r >> 8) & (Z80_XF | Z80_YF))
	          | (((a ^ b ^ r) >> 8) & Z80_HF);
	SETF(f);
	return r;
}

static inline void
adc16(z80 *z, uint16_t v)
{
	uint16_t a = HL;
	unsigned r = a + v + (z->f & Z80_CF);
	z->wz = a + 1;
	uint8_t f = ((r >> 16) & Z80_CF)
	          | ((r >> 8) & (Z80_SF | Z80_XF | Z80_YF))
	          | (((a ^ v ^ r) >> 8) & Z80_HF)
	          | ((((a ^ ~v) & (a ^ r)) >> 13) & Z80_PF);
	if ((r & 0xffff) == 0)
		f |= Z80_ZF;
	SET_HL(r);
	SETF(f);
}

static inline void
sbc16(z80 *z, uint16_t v)
{
	uint16_t a = HL;
	unsigned r = a - v - (z->f & Z80_CF);
	z->wz = a + 1;
	uint8_t f = ((r >> 16) & Z80_CF) | Z80_NF
	          | ((r >> 8) & (Z80_SF | Z80_XF | Z80_YF))
	          | (((a ^ v ^ r) >> 8) & Z80_HF)
	          | ((((a ^ v) & (a ^ r)) >> 13) & Z80_PF);
	if ((r & 0xffff) == 0)
		f |= Z80_ZF;
	SET_HL(r);
	SETF(f);
}

/* CB-prefix rotate/shift group, y = operation */
static uint8_t
rot(z80 *z, int y, uint8_t v)
{
	uint8_t r, c;

	switch (y) {
	case 0: c = v >> 7; r = (v << 1) | c; break;			/* RLC */
	case 1: c = v & 1;  r = (v >> 1) | (c << 7); break;		/* RRC */
	case 2: c = v >> 7; r = (v << 1) | (z->f & Z80_CF); break;	/* RL  */
	case 3: c = v & 1;  r = (v >> 1) | ((z->f & Z80_CF) << 7); break; /* RR */
	case 4: c = v >> 7; r = v << 1; break;				/* SLA */
	case 5: c = v & 1;  r = (v >> 1) | (v & 0x80); break;		/* SRA */
	case 6: c = v >> 7; r = (v << 1) | 1; break;			/* SLL */
	default: c = v & 1; r = v >> 1; break;				/* SRL */
	}
	SETF(sz53p[r] | c);
	return r;
}

static inline void
bit(z80 *z, int b, uint8_t v, uint8_t xy)
{
	uint8_t r = v & (1 << b);
	uint8_t f = (z->f & Z80_CF) | Z80_HF | (xy & (Z80_XF | Z80_YF));
	if (r == 0)
		f |= Z80_ZF | Z80_PF;
	if (b == 7 && r)
		f |= Z80_SF;
	SETF(f);
}

static void
daa(z80 *z)
{
	uint8_t a = z->a, corr = 0, c = z->f & Z80_CF;

	if ((z->f & Z80_HF) || (a & 0x0f) > 9)
		corr |= 0x06;
	if (c || a > 0x99) {
		corr |= 0x60;
		c = Z80_CF;
	}
	uint8_t r;
	uint8_t h;
	if (z->f & Z80_NF) {
		h = (z->f & Z80_HF) && (a & 0x0f) < 6 ? Z80_HF : 0;
		r = a - corr;
	} else {
		h = (a & 0x0f) > 9 ? Z80_HF : 0;
		r = a + corr;
	}
	z->a = r;
	SETF(sz53p[r] | h | c | (z->f & Z80_NF));
}

/* ---------------------------------------------------------------- CB */

/* register access with index substitution for H/L (IXH/IXL etc.) */
static inline uint8_t
get_r(z80 *z, int r, uint16_t *xr)
{
	switch (r) {
	case 0: return z->b;
	case 1: return z->c;
	case 2: return z->d;
	case 3: return z->e;
	case 4: return xr ? *xr >> 8 : z->h;
	case 5: return xr ? *xr & 0xff : z->l;
	default: return z->a;
	}
}

static inline void
set_r(z80 *z, int r, uint16_t *xr, uint8_t v)
{
	switch (r) {
	case 0: z->b = v; break;
	case 1: z->c = v; break;
	case 2: z->d = v; break;
	case 3: z->e = v; break;
	case 4: if (xr) *xr = (*xr & 0x00ff) | (v << 8); else z->h = v; break;
	case 5: if (xr) *xr = (*xr & 0xff00) | v; else z->l = v; break;
	default: z->a = v; break;
	}
}

static unsigned
exec_cb(z80 *z)
{
	uint8_t op = fetch_m1(z);
	int x = op >> 6, y = (op >> 3) & 7, r = op & 7;

	if (r == 6) {
		uint16_t a = HL;
		uint8_t v = rd(z, a);
		switch (x) {
		case 0: wr(z, a, rot(z, y, v)); return 15;
		case 1: bit(z, y, v, z->wz >> 8); return 12;
		case 2: wr(z, a, v & ~(1 << y)); return 15;
		default: wr(z, a, v | (1 << y)); return 15;
		}
	}
	uint8_t v = get_r(z, r, 0);
	switch (x) {
	case 0: set_r(z, r, 0, rot(z, y, v)); break;
	case 1: bit(z, y, v, v); break;
	case 2: set_r(z, r, 0, v & ~(1 << y)); break;
	default: set_r(z, r, 0, v | (1 << y)); break;
	}
	return 8;
}

/* DD CB d op / FD CB d op: d and op are ordinary reads (no M1) */
static unsigned
exec_xycb(z80 *z, uint16_t xr)
{
	int8_t d = (int8_t)fetch8(z);
	uint8_t op = fetch8(z);
	int x = op >> 6, y = (op >> 3) & 7, r = op & 7;
	uint16_t a = xr + d;
	uint8_t v = rd(z, a), res;

	z->wz = a;
	switch (x) {
	case 0: res = rot(z, y, v); break;
	case 1: bit(z, y, v, a >> 8); return 20;
	case 2: res = v & ~(1 << y); break;
	default: res = v | (1 << y); break;
	}
	wr(z, a, res);
	if (r != 6)
		set_r(z, r, 0, res);	/* undocumented copy to register */
	return 23;
}

/* ---------------------------------------------------------------- ED */

static inline uint8_t
in_c(z80 *z)
{
	uint8_t v = z->in(z->ctx, BC);
	z->wz = BC + 1;
	SETF((z->f & Z80_CF) | sz53p[v]);
	return v;
}

static void
ldx(z80 *z, int dir)
{
	uint8_t v = rd(z, HL);
	wr(z, DE, v);
	SET_HL(HL + dir);
	SET_DE(DE + dir);
	SET_BC(BC - 1);
	uint8_t n = v + z->a;
	uint8_t f = (z->f & (Z80_SF | Z80_ZF | Z80_CF))
	          | (n & Z80_XF) | ((n << 4) & Z80_YF);
	if (BC)
		f |= Z80_PF;
	SETF(f);
}

static void
cpx(z80 *z, int dir)
{
	uint8_t v = rd(z, HL);
	uint8_t r = z->a - v;
	uint8_t f = (z->f & Z80_CF) | Z80_NF | (sz53[r] & (Z80_SF | Z80_ZF));
	f |= (z->a ^ v ^ r) & Z80_HF;
	uint8_t n = r - ((f & Z80_HF) ? 1 : 0);
	f |= (n & Z80_XF) | ((n << 4) & Z80_YF);
	SET_HL(HL + dir);
	SET_BC(BC - 1);
	if (BC)
		f |= Z80_PF;
	z->wz += dir;
	SETF(f);
}

/* flags for INI/IND/OUTI/OUTD (after B decremented) */
static void
io_block_flags(z80 *z, uint8_t v, unsigned k)
{
	uint8_t f = sz53[z->b];
	if (v & 0x80)
		f |= Z80_NF;
	if (k > 255)
		f |= Z80_HF | Z80_CF;
	f |= sz53p[(k & 7) ^ z->b] & Z80_PF;
	SETF(f);
}

static void
inx(z80 *z, int dir)
{
	uint8_t v = z->in(z->ctx, BC);
	z->wz = BC + dir;
	wr(z, HL, v);
	z->b--;
	SET_HL(HL + dir);
	io_block_flags(z, v, (unsigned)v + (uint8_t)(z->c + dir));
}

static void
outx(z80 *z, int dir)
{
	uint8_t v = rd(z, HL);
	z->b--;
	z->wz = BC + dir;
	z->out(z->ctx, BC, v);
	SET_HL(HL + dir);
	io_block_flags(z, v, (unsigned)v + z->l);
}

/* repeat-variant flag adjustment when the instruction loops (PC -= 2) */
static void
block_repeat_xy(z80 *z)
{
	z->wz = z->pc + 1;
	z->f = (z->f & ~(Z80_XF | Z80_YF)) | (((z->pc + 1) >> 8) & (Z80_XF | Z80_YF));
	z->q = z->f;
}

static void
io_repeat_flags(z80 *z)
{
	/* undocumented extra flag effects when INIR/OTIR etc. repeat */
	uint8_t f = z->f;
	f = (f & ~(Z80_XF | Z80_YF)) | ((z->pc >> 8) & (Z80_XF | Z80_YF));
	if (f & Z80_CF) {
		uint8_t b = z->b;
		if (z->f & Z80_NF) {
			f ^= sz53p[(b - 1) & 7] & Z80_PF;
			f = (f & ~Z80_HF) | (((b & 0x0f) == 0) ? Z80_HF : 0);
		} else {
			f ^= sz53p[(b + 1) & 7] & Z80_PF;
			f = (f & ~Z80_HF) | (((b & 0x0f) == 0x0f) ? Z80_HF : 0);
		}
	} else {
		f ^= sz53p[z->b & 7] & Z80_PF;
	}
	z->f = f;
	z->q = f;
}

static unsigned
exec_ed(z80 *z)
{
	uint8_t op = fetch_m1(z);
	int y = (op >> 3) & 7;
	uint16_t nn;

	if (op >= 0x40 && op < 0x80) {
		switch (op & 7) {
		case 0: {			/* IN r,(C) */
			uint8_t v = in_c(z);
			if (y != 6)
				set_r(z, y, 0, v);
			return 12;
		}
		case 1:				/* OUT (C),r */
			z->out(z->ctx, BC, y == 6 ? 0 : get_r(z, y, 0));
			z->wz = BC + 1;
			return 12;
		case 2: {			/* SBC/ADC HL,rr */
			int p = y >> 1;
			uint16_t v = p == 0 ? BC : p == 1 ? DE : p == 2 ? HL : z->sp;
			if (y & 1)
				adc16(z, v);
			else
				sbc16(z, v);
			return 15;
		}
		case 3: {			/* LD (nn),rr / LD rr,(nn) */
			int p = y >> 1;
			nn = fetch16(z);
			z->wz = nn + 1;
			if (y & 1) {
				uint16_t v = rd16(z, nn);
				switch (p) {
				case 0: SET_BC(v); break;
				case 1: SET_DE(v); break;
				case 2: SET_HL(v); break;
				default: z->sp = v; break;
				}
			} else {
				uint16_t v = p == 0 ? BC : p == 1 ? DE : p == 2 ? HL : z->sp;
				wr16(z, nn, v);
			}
			return 20;
		}
		case 4: {			/* NEG */
			uint8_t a = z->a;
			z->a = 0;
			sub8(z, a, 0);
			return 8;
		}
		case 5:				/* RETN / RETI */
			z->pc = pop(z);
			z->wz = z->pc;
			z->iff1 = z->iff2;
			if (y == 1 && z->reti)
				z->reti(z->ctx);
			return 14;
		case 6:				/* IM */
			z->im = (y & 3) == 0 ? 0 : (y & 3) == 1 ? 0 : (y & 3) == 2 ? 1 : 2;
			return 8;
		default:
			switch (y) {
			case 0: z->i = z->a; return 9;
			case 1: z->r = z->a; return 9;
			case 2:			/* LD A,I */
			case 3: {		/* LD A,R */
				z->a = y == 2 ? z->i : z->r;
				SETF((z->f & Z80_CF) | sz53[z->a] | (z->iff2 ? Z80_PF : 0));
				return 9;
			}
			case 4: {		/* RRD */
				uint8_t v = rd(z, HL);
				wr(z, HL, (z->a << 4) | (v >> 4));
				z->a = (z->a & 0xf0) | (v & 0x0f);
				SETF((z->f & Z80_CF) | sz53p[z->a]);
				z->wz = HL + 1;
				return 18;
			}
			case 5: {		/* RLD */
				uint8_t v = rd(z, HL);
				wr(z, HL, (v << 4) | (z->a & 0x0f));
				z->a = (z->a & 0xf0) | (v >> 4);
				SETF((z->f & Z80_CF) | sz53p[z->a]);
				z->wz = HL + 1;
				return 18;
			}
			default:
				return 8;	/* ED 77 / ED 7F: NOP */
			}
		}
	}

	switch (op) {
	case 0xa0: ldx(z, 1); return 16;
	case 0xa8: ldx(z, -1); return 16;
	case 0xb0:
	case 0xb8:
		ldx(z, op == 0xb0 ? 1 : -1);
		if (BC) {
			z->pc -= 2;
			block_repeat_xy(z);
			return 21;
		}
		return 16;
	case 0xa1: cpx(z, 1); return 16;
	case 0xa9: cpx(z, -1); return 16;
	case 0xb1:
	case 0xb9:
		cpx(z, op == 0xb1 ? 1 : -1);
		if (BC && !(z->f & Z80_ZF)) {
			z->pc -= 2;
			block_repeat_xy(z);
			return 21;
		}
		return 16;
	case 0xa2: inx(z, 1); return 16;
	case 0xaa: inx(z, -1); return 16;
	case 0xb2:
	case 0xba:
		inx(z, op == 0xb2 ? 1 : -1);
		if (z->b) {
			z->pc -= 2;
			io_repeat_flags(z);
			return 21;
		}
		return 16;
	case 0xa3: outx(z, 1); return 16;
	case 0xab: outx(z, -1); return 16;
	case 0xb3:
	case 0xbb:
		outx(z, op == 0xb3 ? 1 : -1);
		if (z->b) {
			z->pc -= 2;
			io_repeat_flags(z);
			return 21;
		}
		return 16;
	}
	return 8;	/* undefined ED xx: 8T NOP */
}

/* ---------------------------------------------------------------- main */

static inline int
cond(z80 *z, int y)
{
	switch (y) {
	case 0: return !(z->f & Z80_ZF);
	case 1: return z->f & Z80_ZF;
	case 2: return !(z->f & Z80_CF);
	case 3: return z->f & Z80_CF;
	case 4: return !(z->f & Z80_PF);
	case 5: return z->f & Z80_PF;
	case 6: return !(z->f & Z80_SF);
	default: return z->f & Z80_SF;
	}
}

/*
 * Execute an unprefixed opcode, or a DD/FD-prefixed one when xr != NULL.
 * Returns T-states excluding the prefix's 4T.
 */
static unsigned
exec_main(z80 *z, uint8_t op, uint16_t *xr)
{
	unsigned cyc = cyc_main[op];
	int x = op >> 6, y = (op >> 3) & 7, zz = op & 7, p = y >> 1, q = y & 1;
	uint16_t hl = xr ? *xr : HL;
	uint16_t addr, nn;
	uint8_t v;

/* effective address for (HL) / (IX+d) operands */
#define MEMADDR() (xr ? (cyc += 8, z->wz = (uint16_t)(*xr + (int8_t)fetch8(z))) : HL)
#define SET_HLX(v) do { if (xr) *xr = (v); else SET_HL(v); } while (0)

	switch (x) {
	case 1:
		if (op == 0x76) {		/* HALT */
			z->halted = 1;
			return cyc;
		}
		if (y == 6) {			/* LD (HL),r */
			addr = MEMADDR();
			wr(z, addr, get_r(z, zz, 0));
		} else if (zz == 6) {		/* LD r,(HL) */
			addr = MEMADDR();
			set_r(z, y, 0, rd(z, addr));
		} else {
			set_r(z, y, xr, get_r(z, zz, xr));
		}
		return cyc;
	case 2:
		if (zz == 6) {
			addr = MEMADDR();
			v = rd(z, addr);
		} else
			v = get_r(z, zz, xr);
		alu(z, y, v);
		return cyc;
	}

	switch (op) {
	/* ---- x == 0 ---- */
	case 0x00: return cyc;					/* NOP */
	case 0x08: {						/* EX AF,AF' */
		uint8_t t;
		t = z->a; z->a = z->a_; z->a_ = t;
		t = z->f; z->f = z->f_; z->f_ = t;
		return cyc;
	}
	case 0x10:						/* DJNZ */
		v = fetch8(z);
		if (--z->b) {
			z->pc += (int8_t)v;
			z->wz = z->pc;
			return 13;
		}
		return cyc;
	case 0x18:						/* JR */
		v = fetch8(z);
		z->pc += (int8_t)v;
		z->wz = z->pc;
		return cyc;
	case 0x20: case 0x28: case 0x30: case 0x38:		/* JR cc */
		v = fetch8(z);
		if (cond(z, y - 4)) {
			z->pc += (int8_t)v;
			z->wz = z->pc;
			return 12;
		}
		return cyc;
	case 0x01: SET_BC(fetch16(z)); return cyc;
	case 0x11: SET_DE(fetch16(z)); return cyc;
	case 0x21: SET_HLX(fetch16(z)); return cyc;
	case 0x31: z->sp = fetch16(z); return cyc;
	case 0x09: case 0x19: case 0x29: case 0x39: {		/* ADD HL,rr */
		uint16_t rr = p == 0 ? BC : p == 1 ? DE : p == 2 ? hl : z->sp;
		SET_HLX(add16(z, hl, rr));
		return cyc;
	}
	case 0x02: wr(z, BC, z->a); z->wz = ((BC + 1) & 0xff) | (z->a << 8); return cyc;
	case 0x12: wr(z, DE, z->a); z->wz = ((DE + 1) & 0xff) | (z->a << 8); return cyc;
	case 0x0a: z->a = rd(z, BC); z->wz = BC + 1; return cyc;
	case 0x1a: z->a = rd(z, DE); z->wz = DE + 1; return cyc;
	case 0x22: nn = fetch16(z); wr16(z, nn, hl); z->wz = nn + 1; return cyc;
	case 0x2a: nn = fetch16(z); SET_HLX(rd16(z, nn)); z->wz = nn + 1; return cyc;
	case 0x32: nn = fetch16(z); wr(z, nn, z->a); z->wz = ((nn + 1) & 0xff) | (z->a << 8); return cyc;
	case 0x3a: nn = fetch16(z); z->a = rd(z, nn); z->wz = nn + 1; return cyc;
	case 0x03: SET_BC(BC + 1); return cyc;
	case 0x13: SET_DE(DE + 1); return cyc;
	case 0x23: SET_HLX(hl + 1); return cyc;
	case 0x33: z->sp++; return cyc;
	case 0x0b: SET_BC(BC - 1); return cyc;
	case 0x1b: SET_DE(DE - 1); return cyc;
	case 0x2b: SET_HLX(hl - 1); return cyc;
	case 0x3b: z->sp--; return cyc;
	case 0x34: addr = MEMADDR(); wr(z, addr, inc8(z, rd(z, addr))); return cyc;
	case 0x35: addr = MEMADDR(); wr(z, addr, dec8(z, rd(z, addr))); return cyc;
	case 0x04: case 0x0c: case 0x14: case 0x1c: case 0x24: case 0x2c: case 0x3c:
		set_r(z, y, xr, inc8(z, get_r(z, y, xr)));
		return cyc;
	case 0x05: case 0x0d: case 0x15: case 0x1d: case 0x25: case 0x2d: case 0x3d:
		set_r(z, y, xr, dec8(z, get_r(z, y, xr)));
		return cyc;
	case 0x36:						/* LD (HL),n */
		if (xr) {
			addr = z->wz = *xr + (int8_t)fetch8(z);
			cyc += 5;
		} else
			addr = HL;
		wr(z, addr, fetch8(z));
		return cyc;
	case 0x06: case 0x0e: case 0x16: case 0x1e: case 0x26: case 0x2e: case 0x3e:
		set_r(z, y, xr, fetch8(z));
		return cyc;
	case 0x07: {						/* RLCA */
		z->a = (z->a << 1) | (z->a >> 7);
		SETF((z->f & (Z80_SF | Z80_ZF | Z80_PF)) | (z->a & (Z80_XF | Z80_YF | Z80_CF)));
		return cyc;
	}
	case 0x0f: {						/* RRCA */
		uint8_t c = z->a & 1;
		z->a = (z->a >> 1) | (c << 7);
		SETF((z->f & (Z80_SF | Z80_ZF | Z80_PF)) | (z->a & (Z80_XF | Z80_YF)) | c);
		return cyc;
	}
	case 0x17: {						/* RLA */
		uint8_t c = z->a >> 7;
		z->a = (z->a << 1) | (z->f & Z80_CF);
		SETF((z->f & (Z80_SF | Z80_ZF | Z80_PF)) | (z->a & (Z80_XF | Z80_YF)) | c);
		return cyc;
	}
	case 0x1f: {						/* RRA */
		uint8_t c = z->a & 1;
		z->a = (z->a >> 1) | ((z->f & Z80_CF) << 7);
		SETF((z->f & (Z80_SF | Z80_ZF | Z80_PF)) | (z->a & (Z80_XF | Z80_YF)) | c);
		return cyc;
	}
	case 0x27: daa(z); return cyc;
	case 0x2f:						/* CPL */
		z->a = ~z->a;
		SETF((z->f & (Z80_SF | Z80_ZF | Z80_PF | Z80_CF)) | Z80_HF | Z80_NF
		     | (z->a & (Z80_XF | Z80_YF)));
		return cyc;
	case 0x37:						/* SCF */
		SETF((z->f & (Z80_SF | Z80_ZF | Z80_PF)) | Z80_CF
		     | (((z->lastq ^ z->f) | z->a) & (Z80_XF | Z80_YF)));
		return cyc;
	case 0x3f: {						/* CCF */
		uint8_t c = z->f & Z80_CF;
		SETF(((z->f & (Z80_SF | Z80_ZF | Z80_PF)) | (c ? Z80_HF : Z80_CF))
		     | (((z->lastq ^ z->f) | z->a) & (Z80_XF | Z80_YF)));
		return cyc;
	}

	/* ---- x == 3 ---- */
	case 0xc0: case 0xc8: case 0xd0: case 0xd8:
	case 0xe0: case 0xe8: case 0xf0: case 0xf8:		/* RET cc */
		if (cond(z, y)) {
			z->pc = pop(z);
			z->wz = z->pc;
			return 11;
		}
		return cyc;
	case 0xc1: SET_BC(pop(z)); return cyc;
	case 0xd1: SET_DE(pop(z)); return cyc;
	case 0xe1: SET_HLX(pop(z)); return cyc;
	case 0xf1: SET_AF(pop(z)); return cyc;
	case 0xc9: z->pc = pop(z); z->wz = z->pc; return cyc;	/* RET */
	case 0xd9: {						/* EXX */
		uint8_t t;
		t = z->b; z->b = z->b_; z->b_ = t;
		t = z->c; z->c = z->c_; z->c_ = t;
		t = z->d; z->d = z->d_; z->d_ = t;
		t = z->e; z->e = z->e_; z->e_ = t;
		t = z->h; z->h = z->h_; z->h_ = t;
		t = z->l; z->l = z->l_; z->l_ = t;
		return cyc;
	}
	case 0xe9: z->pc = hl; return cyc;			/* JP (HL) */
	case 0xf9: z->sp = hl; return cyc;			/* LD SP,HL */
	case 0xc2: case 0xca: case 0xd2: case 0xda:
	case 0xe2: case 0xea: case 0xf2: case 0xfa:		/* JP cc,nn */
		nn = fetch16(z);
		z->wz = nn;
		if (cond(z, y))
			z->pc = nn;
		return cyc;
	case 0xc3: nn = fetch16(z); z->pc = z->wz = nn; return cyc;
	case 0xd3:						/* OUT (n),A */
		v = fetch8(z);
		z->out(z->ctx, (z->a << 8) | v, z->a);
		z->wz = ((v + 1) & 0xff) | (z->a << 8);
		return cyc;
	case 0xdb: {						/* IN A,(n) */
		v = fetch8(z);
		uint16_t port = (z->a << 8) | v;
		z->a = z->in(z->ctx, port);
		z->wz = port + 1;
		return cyc;
	}
	case 0xe3: {						/* EX (SP),HL */
		uint16_t t = rd16(z, z->sp);
		wr16(z, z->sp, hl);
		SET_HLX(t);
		z->wz = t;
		return cyc;
	}
	case 0xeb: {						/* EX DE,HL (never IX) */
		uint8_t t;
		t = z->d; z->d = z->h; z->h = t;
		t = z->e; z->e = z->l; z->l = t;
		return cyc;
	}
	case 0xf3: z->iff1 = z->iff2 = 0; return cyc;		/* DI */
	case 0xfb:						/* EI */
		z->iff1 = z->iff2 = 1;
		z->ei_pending = 1;
		return cyc;
	case 0xc4: case 0xcc: case 0xd4: case 0xdc:
	case 0xe4: case 0xec: case 0xf4: case 0xfc:		/* CALL cc,nn */
		nn = fetch16(z);
		z->wz = nn;
		if (cond(z, y)) {
			push(z, z->pc);
			z->pc = nn;
			return 17;
		}
		return cyc;
	case 0xc5: push(z, BC); return cyc;
	case 0xd5: push(z, DE); return cyc;
	case 0xe5: push(z, hl); return cyc;
	case 0xf5: push(z, AF); return cyc;
	case 0xcd:						/* CALL nn */
		nn = fetch16(z);
		push(z, z->pc);
		z->pc = z->wz = nn;
		return cyc;
	case 0xc6: case 0xce: case 0xd6: case 0xde:
	case 0xe6: case 0xee: case 0xf6: case 0xfe:
		alu(z, y, fetch8(z));
		return cyc;
	case 0xc7: case 0xcf: case 0xd7: case 0xdf:
	case 0xe7: case 0xef: case 0xf7: case 0xff:		/* RST */
		push(z, z->pc);
		z->pc = z->wz = y * 8;
		return cyc;
	}
	(void)q;
	return cyc;
#undef MEMADDR
#undef SET_HLX
}

/* ---------------------------------------------------------------- step */

void
z80_reset(z80 *z)
{
	if (!tables_done)
		init_tables();
	z->pc = 0;
	z->i = z->r = 0;
	z->iff1 = z->iff2 = 0;
	z->im = 0;
	z->halted = 0;
	z->ei_pending = 0;
	z->nmi_pending = 0;
	z->a = z->f = 0xff;
	z->sp = 0xffff;
	z->q = z->lastq = 0;
}

unsigned
z80_step(z80 *z)
{
	unsigned cyc;

	z->m1 = 0;
	z->lastq = z->q;
	z->q = 0;

	if (z->nmi_pending) {
		z->nmi_pending = 0;
		z->halted = 0;
		z->m1 = 1;
		z->r = (z->r & 0x80) | ((z->r + 1) & 0x7f);
		z->iff1 = 0;
		push(z, z->pc);
		z->pc = z->wz = 0x66;
		z->ei_pending = 0;
		return z->t = 11;
	}
	if (z->int_line && z->iff1 && !z->ei_pending) {
		z->halted = 0;
		z->iff1 = z->iff2 = 0;
		z->m1 = 1;
		z->r = (z->r & 0x80) | ((z->r + 1) & 0x7f);
		uint8_t vec = z->int_ack ? z->int_ack(z->ctx) : 0xff;
		push(z, z->pc);
		switch (z->im) {
		case 2:
			z->pc = rd16(z, (z->i << 8) | vec);
			cyc = 19;
			break;
		case 1:
			z->pc = 0x38;
			cyc = 13;
			break;
		default:	/* IM 0: assume RST on the bus */
			z->pc = vec & 0x38;
			cyc = 13;
			break;
		}
		z->wz = z->pc;
		return z->t = cyc;
	}
	z->ei_pending = 0;

	if (z->halted) {
		z->m1 = 1;
		z->r = (z->r & 0x80) | ((z->r + 1) & 0x7f);
		return z->t = 4;
	}

	uint8_t op = fetch_m1(z);
	cyc = 0;
	for (;;) {
		switch (op) {
		case 0xcb:
			return z->t = cyc + exec_cb(z);
		case 0xed:
			return z->t = cyc + exec_ed(z);
		case 0xdd:
		case 0xfd: {
			uint16_t *xr = op == 0xdd ? &z->ix : &z->iy;
			cyc += 4;
			op = fetch_m1(z);
			if (op == 0xdd || op == 0xfd)
				continue;	/* prefix chain: previous one is a NOP */
			if (op == 0xed)
				continue;	/* DD ED: DD acts as a NOP */
			if (op == 0xcb)
				return z->t = cyc + exec_xycb(z, *xr);
			/* EX DE,HL and ops not touching HL ignore the prefix */
			return z->t = cyc + exec_main(z, op, xr);
		}
		default:
			return z->t = cyc + exec_main(z, op, 0);
		}
	}
}
