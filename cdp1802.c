/*
 * CDP1802 / CDP1806 CPU core.  See cdp1802.h.
 *
 * Instruction semantics: RCA MPM-201A (1802) and the Harris CDP1805AC/
 * 1806AC data sheet (counter/timer, interrupt logic).  The 68xx cycle
 * counts are the CDP1805 user manual's (3, DBNZ 5, RLXA 5, DSAV 6,
 * SCAL 10, SRET 8, RSXD 5, RNX 4, RLDI 5, BCD 4).
 */
#include "cdp1802.h"

#include <string.h>

#define RD(a)    c->read(c->ctx, (uint16_t)(a))
#define WR(a, v) c->write(c->ctx, (uint16_t)(a), (uint8_t)(v))
#define RP       c->r[c->p]
#define RX       c->r[c->x]

static void
set_q(cdp1802 *c, int q)
{
	q = !!q;
	if (q != c->q) {
		c->q = (uint8_t)q;
		if (c->qout)
			c->qout(c->ctx, q);
	}
}

void
cdp1802_reset(cdp1802 *c, int type)
{
	/* D, DF, R1-R15 are not affected by /CLEAR */
	c->type = (uint8_t)type;
	c->t = (uint8_t)((c->x << 4) | c->p);
	c->x = c->p = 0;
	c->r[0] = 0;
	set_q(c, 0);
	c->ie = 1;
	c->idle = 0;
	c->xie = c->cie = 1;
	c->cil = 0;
	c->ctmode = CDP_CT_STOP;
	c->prescale = 0;
	c->etq = 0;
	c->cycles = 0;
	c->illegal = 0;
}

/* ---------------------------------------------------------------- ALU */

static void
add(cdp1802 *c, uint8_t m, int cin)
{
	unsigned s = (unsigned)c->d + m + (unsigned)cin;
	c->d = (uint8_t)s;
	c->df = s > 0xff;
}

/* D = a - b - borrow; DF = no borrow */
static uint8_t
sub(cdp1802 *c, uint8_t a, uint8_t b, int borrow)
{
	int s = (int)a - b - borrow;
	c->df = s >= 0;
	return (uint8_t)s;
}

static void
dadd(cdp1802 *c, uint8_t m, int cin)
{
	unsigned lo = (c->d & 0x0f) + (m & 0x0f) + (unsigned)cin;
	unsigned s = (unsigned)c->d + m + (unsigned)cin;
	if (lo > 9)
		s += 6;
	if ((s & 0x1f0) > 0x90)
		s += 0x60;
	c->d = (uint8_t)s;
	c->df = s > 0xff;
}

/* D = a - b decimal */
static void
dsub(cdp1802 *c, uint8_t a, uint8_t b, int borrow)
{
	int lo = (a & 0x0f) - (b & 0x0f) - borrow;
	int s = (int)a - b - borrow;
	if (lo < 0)
		s -= 6;
	c->df = s >= 0;
	if (!c->df)
		s -= 0x60;
	c->d = (uint8_t)s;
}

/* ---------------------------------------------------------------- counter */

static void
ct_count(cdp1802 *c)
{
	if (c->cntr == 1) {
		c->cntr = c->ch;
		c->cil = 1;
		if (c->etq)
			set_q(c, !c->q);
	} else {
		c->cntr--;
	}
}

/* advance the counter by n machine cycles (n TPA pulses) */
static void
ct_cycles(cdp1802 *c, unsigned n)
{
	if (c->ctmode == CDP_CT_TIMER) {
		while (n--) {
			if (++c->prescale == 32) {
				c->prescale = 0;
				ct_count(c);
			}
		}
	} else if (c->ctmode == CDP_CT_PULSE1 || c->ctmode == CDP_CT_PULSE2) {
		unsigned bit = c->ctmode == CDP_CT_PULSE1 ? 1 : 2;
		if (c->ef_last & bit)	/* EF true = pin low = gate open */
			while (n--)
				ct_count(c);
	}
}

/* EF1/EF2 edges, sampled after each step */
static void
ct_flags(cdp1802 *c)
{
	if (c->ctmode == CDP_CT_STOP || c->ctmode == CDP_CT_TIMER || !c->ef)
		return;
	uint8_t ef = c->ef(c->ctx) & 3;
	uint8_t fell = ef & ~c->ef_last;	/* pin high-to-low = flag 0->1 */
	uint8_t rose = c->ef_last & ~ef;
	switch (c->ctmode) {
	case CDP_CT_EVENT1: if (fell & 1) ct_count(c); break;
	case CDP_CT_EVENT2: if (fell & 2) ct_count(c); break;
	case CDP_CT_PULSE1:
	case CDP_CT_PULSE2:
		if (rose & (c->ctmode == CDP_CT_PULSE1 ? 1 : 2)) {
			c->ctmode = CDP_CT_STOP;
			c->cil = 1;
		}
		break;
	}
	c->ef_last = ef;
}

static void
ct_setmode(cdp1802 *c, int mode)
{
	c->ctmode = (uint8_t)mode;
	c->prescale = 0;
	if (c->ef)
		c->ef_last = c->ef(c->ctx) & 3;
}

/* ---------------------------------------------------------------- step */

static int
int_pending(const cdp1802 *c)
{
	if (c->type == CDP1802)
		return c->int_line;
	return (c->int_line && c->xie) || (c->cil && c->cie);
}

static void
lbranch(cdp1802 *c, int cond)
{
	if (cond) {
		uint8_t hi = RD(RP), lo = RD(RP + 1);
		RP = (uint16_t)((hi << 8) | lo);
	} else {
		RP += 2;
	}
}

static void
sbranch(cdp1802 *c, int cond)
{
	if (cond)
		RP = (uint16_t)((RP & 0xff00) | RD(RP));
	else
		RP++;
}

static uint8_t
flags(cdp1802 *c)
{
	return c->ef ? c->ef(c->ctx) : 0;
}

/* 68xx; returns machine cycles */
static unsigned
step68(cdp1802 *c)
{
	uint8_t op = RD(RP++);
	int n = op & 15;
	uint16_t a;

	switch (op >> 4) {
	case 0x0:
		switch (n) {
		case 0x0: c->ctmode = CDP_CT_STOP; c->prescale = 0; return 3;	/* STPC */
		case 0x1: ct_count(c); return 3;				/* DTC */
		case 0x2: ct_setmode(c, CDP_CT_PULSE2); return 3;		/* SPM2 */
		case 0x3: ct_setmode(c, CDP_CT_EVENT2); return 3;		/* SCM2 */
		case 0x4: ct_setmode(c, CDP_CT_PULSE1); return 3;		/* SPM1 */
		case 0x5: ct_setmode(c, CDP_CT_EVENT1); return 3;		/* SCM1 */
		case 0x6:							/* LDC */
			if (c->ctmode == CDP_CT_STOP) {
				c->cntr = c->ch = c->d;
				c->cil = 0;
				c->etq = 0;
			} else {
				c->ch = c->d;
			}
			return 3;
		case 0x7: ct_setmode(c, CDP_CT_TIMER); return 3;		/* STM */
		case 0x8: c->d = c->cntr; return 3;				/* GEC */
		case 0x9: c->etq = 1; return 3;					/* ETQ */
		case 0xa: c->xie = 1; return 3;					/* XIE */
		case 0xb: c->xie = 0; return 3;					/* XID */
		case 0xc: c->cie = 1; return 3;					/* CIE */
		case 0xd: c->cie = 0; return 3;					/* CID */
		}
		break;
	case 0x2:							/* DBNZ */
		c->r[n]--;
		lbranch(c, c->r[n] != 0);
		return 5;
	case 0x3:
		if (n == 0xe) {						/* BCI */
			int t = c->cil;
			if (t) {
				c->cil = 0;
				c->etq = 0;
			}
			sbranch(c, t);
			return 3;
		}
		if (n == 0xf) {						/* BXI */
			sbranch(c, c->int_line);
			return 3;
		}
		break;
	case 0x6:							/* RLXA */
		a = RX;
		c->r[n] = (uint16_t)((RD(a) << 8) | RD(a + 1));
		RX += 2;
		return 5;
	case 0x7:
		switch (n) {
		case 0x4: dadd(c, RD(RX), c->df); return 4;		/* DADC */
		case 0x6:						/* DSAV */
			RX--; WR(RX, c->t);
			RX--; WR(RX, c->d);
			RX--;
			{
				uint8_t d = c->d;
				c->d = (uint8_t)((d >> 1) | (c->df << 7));
				c->df = d & 1;
			}
			WR(RX, c->d);
			return 6;
		case 0x7: dsub(c, c->d, RD(RX), !c->df); return 4;	/* DSMB */
		case 0xc: dadd(c, RD(RP++), c->df); return 4;		/* DACI */
		case 0xf: dsub(c, c->d, RD(RP++), !c->df); return 4;	/* DSBI */
		}
		break;
	case 0x8:							/* SCAL */
		WR(RX, c->r[n] & 0xff);
		WR(RX - 1, c->r[n] >> 8);
		RX -= 2;
		c->r[n] = RP;
		RP = (uint16_t)((RD(c->r[n]) << 8) | RD(c->r[n] + 1));
		c->r[n] += 2;
		return 10;
	case 0x9:							/* SRET */
		RP = c->r[n];
		c->r[n] = (uint16_t)((RD(RX + 1) << 8) | RD(RX + 2));
		RX += 2;
		return 8;
	case 0xa:							/* RSXD */
		WR(RX, c->r[n] & 0xff);
		WR(RX - 1, c->r[n] >> 8);
		RX -= 2;
		return 5;
	case 0xb:							/* RNX */
		RX = c->r[n];
		return 4;
	case 0xc:							/* RLDI */
		c->r[n] = (uint16_t)((RD(RP) << 8) | RD(RP + 1));
		RP += 2;
		return 5;
	case 0xf:
		switch (n) {
		case 0x4: dadd(c, RD(RX), 0); return 4;			/* DADD */
		case 0x7: dsub(c, c->d, RD(RX), 0); return 4;		/* DSM */
		case 0xc: dadd(c, RD(RP++), 0); return 4;		/* DADI */
		case 0xf: dsub(c, c->d, RD(RP++), 0); return 4;		/* DSMI */
		}
		break;
	}
	c->illegal = 1;
	return 3;
}

static unsigned
exec(cdp1802 *c)
{
	uint8_t op = RD(RP++);
	int n = op & 15;
	uint8_t m, d;

	switch (op >> 4) {
	case 0x0:
		if (n == 0) {
			c->idle = 1;				/* IDL */
			return 2;
		}
		c->d = RD(c->r[n]);				/* LDN */
		return 2;
	case 0x1: c->r[n]++; return 2;				/* INC */
	case 0x2: c->r[n]--; return 2;				/* DEC */
	case 0x3: {
		uint8_t ef = (n & 4) ? flags(c) : 0;
		int cond;
		switch (n & 7) {
		case 0: cond = 1; break;
		case 1: cond = c->q; break;
		case 2: cond = c->d == 0; break;
		case 3: cond = c->df; break;
		default: cond = (ef >> ((n & 7) - 4)) & 1; break;
		}
		if (n == 8) {					/* SKP */
			RP++;
			return 2;
		}
		sbranch(c, (n & 8) ? !cond : cond);
		return 2;
	}
	case 0x4: c->d = RD(c->r[n]); c->r[n]++; return 2;	/* LDA */
	case 0x5: WR(c->r[n], c->d); return 2;			/* STR */
	case 0x6:
		if (n == 0) {
			RX++;					/* IRX */
		} else if (n < 8) {
			m = RD(RX);				/* OUT */
			if (c->out)
				c->out(c->ctx, n, RX, m);
			RX++;
		} else if (n == 8) {
			if (c->type == CDP1806)
				return step68(c);
			c->illegal = 1;
		} else {
			m = c->in ? c->in(c->ctx, n & 7, RX) : 0xff;	/* INP */
			WR(RX, m);
			c->d = m;
		}
		return 2;
	case 0x7:
		switch (n) {
		case 0x0:					/* RET */
		case 0x1:					/* DIS */
			m = RD(RX);
			RX++;
			c->x = m >> 4;
			c->p = m & 15;
			c->ie = n == 0;
			break;
		case 0x2: c->d = RD(RX); RX++; break;		/* LDXA */
		case 0x3: WR(RX, c->d); RX--; break;		/* STXD */
		case 0x4: add(c, RD(RX), c->df); break;		/* ADC */
		case 0x5: c->d = sub(c, RD(RX), c->d, !c->df); break;	/* SDB */
		case 0x6:					/* SHRC */
			d = c->d;
			c->d = (uint8_t)((d >> 1) | (c->df << 7));
			c->df = d & 1;
			break;
		case 0x7: c->d = sub(c, c->d, RD(RX), !c->df); break;	/* SMB */
		case 0x8: WR(RX, c->t); break;			/* SAV */
		case 0x9:					/* MARK */
			c->t = (uint8_t)((c->x << 4) | c->p);
			WR(c->r[2], c->t);
			c->x = c->p;
			c->r[2]--;
			break;
		case 0xa: set_q(c, 0); break;			/* REQ */
		case 0xb: set_q(c, 1); break;			/* SEQ */
		case 0xc: add(c, RD(RP++), c->df); break;	/* ADCI */
		case 0xd: c->d = sub(c, RD(RP++), c->d, !c->df); break;	/* SDBI */
		case 0xe:					/* SHLC */
			d = c->d;
			c->d = (uint8_t)((d << 1) | c->df);
			c->df = d >> 7;
			break;
		case 0xf: c->d = sub(c, c->d, RD(RP++), !c->df); break;	/* SMBI */
		}
		return 2;
	case 0x8: c->d = (uint8_t)c->r[n]; return 2;		/* GLO */
	case 0x9: c->d = (uint8_t)(c->r[n] >> 8); return 2;	/* GHI */
	case 0xa: c->r[n] = (uint16_t)((c->r[n] & 0xff00) | c->d); return 2;	/* PLO */
	case 0xb: c->r[n] = (uint16_t)((c->r[n] & 0x00ff) | (c->d << 8)); return 2;	/* PHI */
	case 0xc: {
		int cond;
		switch (n & 3) {
		case 0: cond = 1; break;
		case 1: cond = c->q; break;
		case 2: cond = c->d == 0; break;
		default: cond = c->df; break;
		}
		if (n == 0x4) {					/* NOP */
		} else if (n == 0xc) {				/* LSIE */
			if (c->ie)
				RP += 2;
		} else if (n & 4) {
			/* long skips: C5 LSNQ C6 LSNZ C7 LSNF C8 LSKP
			 * CD LSQ CE LSZ CF LSDF */
			if (n == 0x8 || ((n & 8) ? cond : !cond))
				RP += 2;
		} else {
			lbranch(c, (n & 8) ? !cond : cond);	/* LBR.. */
		}
		return 3;
	}
	case 0xd: c->p = (uint8_t)n; return 2;			/* SEP */
	case 0xe: c->x = (uint8_t)n; return 2;			/* SEX */
	case 0xf:
		switch (n) {
		case 0x0: c->d = RD(RX); break;			/* LDX */
		case 0x1: c->d |= RD(RX); break;		/* OR */
		case 0x2: c->d &= RD(RX); break;		/* AND */
		case 0x3: c->d ^= RD(RX); break;		/* XOR */
		case 0x4: add(c, RD(RX), 0); break;		/* ADD */
		case 0x5: c->d = sub(c, RD(RX), c->d, 0); break;	/* SD */
		case 0x6: c->df = c->d & 1; c->d >>= 1; break;	/* SHR */
		case 0x7: c->d = sub(c, c->d, RD(RX), 0); break;	/* SM */
		case 0x8: c->d = RD(RP++); break;		/* LDI */
		case 0x9: c->d |= RD(RP++); break;		/* ORI */
		case 0xa: c->d &= RD(RP++); break;		/* ANI */
		case 0xb: c->d ^= RD(RP++); break;		/* XRI */
		case 0xc: add(c, RD(RP++), 0); break;		/* ADI */
		case 0xd: c->d = sub(c, RD(RP++), c->d, 0); break;	/* SDI */
		case 0xe: c->df = c->d >> 7; c->d <<= 1; break;	/* SHL */
		case 0xf: c->d = sub(c, c->d, RD(RP++), 0); break;	/* SMI */
		}
		return 2;
	}
	return 2;
}

unsigned
cdp1802_step(cdp1802 *c)
{
	unsigned cyc;

	c->illegal = 0;
	if (int_pending(c) && c->ie) {
		c->t = (uint8_t)((c->x << 4) | c->p);
		c->x = 2;
		c->p = 1;
		c->ie = 0;
		c->idle = 0;
		cyc = 1;
	} else if (c->idle) {
		/* an interrupt request ends IDL even with IE = 0 */
		if (int_pending(c))
			c->idle = 0;
		cyc = 1;
	} else {
		cyc = exec(c);
	}
	if (c->type == CDP1806) {
		ct_cycles(c, cyc);
		ct_flags(c);
	}
	c->cycles = cyc;
	return cyc;
}
