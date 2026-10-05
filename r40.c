/*
 * Nokia R40 logic board L100 and its CU43 control head; see r40.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r40.h"

/* port 1 */
#define P1_SCLK  0x04
#define P1_SDOUT 0x08
#define P1_SDIN  0x10
#define P1_IRQ0  0x20
/* port 7 */
#define P7_SD    0x02
#define P7_I2INT 0x04	/* PCF8584 INT -> FTI2 */
#define P7_PTT   0x08	/* /PTT -> FTI3 */
#define P7_CLK   0x10
#define P7_STE   0x40
#define P7_SRE   0x80
/* port 9 */
#define P9_NVSEL 0x04
/* OUT0 */
#define O0_DACCS 0x01	/* active low */
#define O0_RTCCS 0x02	/* active high */
#define O0_FXCS  0x04	/* FX803, active low */
#define O0_2MHZ  0x40
#define O0_504K  0x80
/* OUT1 */
#define O1_TXON  0x01
#define O1_SR0   0x02

/* I2C addresses (8-bit, write) */
#define A_PCF120 0x40
#define A_PCF190 0x42
#define A_PCF200 0x44
#define A_DTMF   0x48
#define A_LCD    0x78
#define A_EEPROM 0xA0

static void
ev(r40 *m, int type, int arg)
{
	r40_event *e;
	if (m->ev_n == R40_EVQ) {
		m->ev_head = (m->ev_head + 1) % R40_EVQ;
		m->ev_n--;
	}
	e = &m->ev[(m->ev_head + m->ev_n++) % R40_EVQ];
	e->at = m->clk;
	e->type = type;
	e->arg = arg;
}

int
r40_event_pop(r40 *m, r40_event *e)
{
	if (!m->ev_n)
		return 0;
	*e = m->ev[m->ev_head];
	m->ev_head = (m->ev_head + 1) % R40_EVQ;
	m->ev_n--;
	return 1;
}

double
r40_time(const r40 *m)
{
	return (double)m->clk / R40_HZ;
}

/* ------------------------------------------------------- control head */

/* PCF8574 IC190 inputs: P7 nc, P6 /ON-OFF, P5 HOOK, P4-0 key columns.
 * Keys join row r (IC200 P r) and column c (IC190 P c); a pressed key pulls
 * both lines low if either side's latch drives low. */
static uint8_t
pcf_pins(r40 *m, int which)
{
	uint8_t col = m->pcf[1], row = m->pcf[2];
	uint8_t cols = col & 0x1F, rows = row & 0x1F;
	int r, c, again = 1;
	while (again) {
		again = 0;
		for (c = 0; c < 5; c++)
			for (r = 0; r < 5; r++) {
				if (!(m->keys[c] & (1 << r)))
					continue;
				if ((cols & (1 << c)) && !(rows & (1 << r))) {
					cols &= (uint8_t)~(1 << c);
					again = 1;
				}
				if ((rows & (1 << r)) && !(cols & (1 << c))) {
					rows &= (uint8_t)~(1 << r);
					again = 1;
				}
			}
	}
	if (which == 1) {
		uint8_t v = (uint8_t)((col & 0xE0) | cols);
		if (m->onoff_key)
			v &= (uint8_t)~0x40;
		if (!m->hook)	/* on hook reads 0 */
			v &= (uint8_t)~0x20;
		return v;
	}
	/* P6 is an input strapped by the head type: 0 = CU43 (the firmware
	 * then drives the dot-matrix LCD; 1 makes it use an 8-character
	 * segment display at I2C 0x74) */
	if (m->head_cu43)
		row &= (uint8_t)~0x40;
	return (uint8_t)((row & 0xE0) | rows);
}

static void
irq0_update(r40 *m)
{
	/* IC190 /INT: low while its inputs differ from what was last read */
	int low = pcf_pins(m, 1) != m->pcf190_seen;
	h8532_pin(&m->chip, 1, 5, !low);
}

/* -------------------------------------------------------------- LCD */

/* PCF8579 (subaddress 0) + 3 x PCF8578: commands until one with C = 0,
 * then display data to the selected device's RAM.  RAM is kept per device
 * as 4 banks x 40 columns of 8 vertical pixels. */
static void
lcd_byte(r40 *m, int idx, uint8_t b)
{
	if (idx == 0)
		m->lcd_cmd = 1;
	if (m->lcd_cmd) {
		int c = b & 0x80;
		uint8_t op = b & 0x7F;
		if ((op & 0x40) == 0)
			m->lcd_x = op & 0x3F;			/* LOAD-X-ADDRESS */
		else if ((op & 0x60) == 0x40)
			m->lcd_mode_set = op;			/* SET-MODE */
		else if ((op & 0x7C) == 0x7C)
			m->lcd_start = op & 3;			/* SET-START-BANK */
		else if ((op & 0x70) == 0x60)
			m->lcd_dev = op & 0x0F;			/* DEVICE-SELECT */
		else if ((op & 0x70) == 0x70) {			/* RAM-ACCESS */
			m->lcd_mode = (op >> 2) & 3;
			m->lcd_bank = op & 3;
		}
		if (!c)
			m->lcd_cmd = 0;
		return;
	}
	if (m->lcd_dev < 4 && m->lcd_x < 40)
		m->lcd[m->lcd_dev][m->lcd_bank][m->lcd_x] = b;
	m->lcd_writes++;
	/* auto-increment: character mode steps X; half / full graphic step
	 * the bank first (one and three extra banks) */
	switch (m->lcd_mode) {
	case 1:
		if (m->lcd_bank & 1) {
			m->lcd_bank--;
			m->lcd_x++;
		} else
			m->lcd_bank++;
		break;
	case 2:
		if (m->lcd_bank == 3) {
			m->lcd_bank = 0;
			m->lcd_x++;
		} else
			m->lcd_bank++;
		break;
	default:
		m->lcd_x++;
	}
	if (m->lcd_x >= 40) {
		m->lcd_x = 0;
		m->lcd_dev++;
	}
}

/* ---------------------------------------------------------------- I2C */

/* a byte the master wrote; returns the slave's ACK (1 = acked) */
static int
i2c_write(r40 *m, uint8_t b)
{
	r40_i2c *t = &m->i2c;
	int idx = t->nbytes;
	if (t->nbytes < R40_I2C_MAX)
		t->buf[t->nbytes] = b;
	t->nbytes++;
	if (idx == 0) {		/* address */
		t->addr = b & 0xFE;
		t->rd = b & 1;
		t->active = 1;
		switch (t->addr) {
		case A_PCF120: case A_PCF190: case A_PCF200:
		case A_LCD: case A_DTMF:
			return 1;

		case A_EEPROM:
			return m->service_head;
		}
		return 0;
	}
	switch (t->addr) {
	case A_PCF120: m->pcf[0] = b; break;
	case A_PCF190: m->pcf[1] = b; m->pcf190_seen = pcf_pins(m, 1); irq0_update(m); break;
	case A_PCF200: m->pcf[2] = b; irq0_update(m); break;
	case A_DTMF: ev(m, R40_EV_DTMF, b); break;
	case A_LCD: lcd_byte(m, idx - 1, b); break;
	case A_EEPROM:
		if (idx == 1)
			m->ee_ptr = b;
		else
			m->eeprom[m->ee_ptr++ & 0xFF] = b;
		break;
	}
	return 1;
}

static uint8_t
i2c_read(r40 *m)
{
	r40_i2c *t = &m->i2c;
	uint8_t v = 0xFF;
	switch (t->addr) {
	case A_PCF120: v = m->pcf[0]; break;
	case A_PCF190:
		v = pcf_pins(m, 1);
		m->pcf190_seen = v;
		irq0_update(m);
		break;
	case A_PCF200: v = pcf_pins(m, 2); break;
	case A_EEPROM:
		if (m->service_head)
			v = m->eeprom[m->ee_ptr++ & 0xFF];
		break;
	}
	if (t->nbytes < R40_I2C_MAX)
		t->buf[t->nbytes] = v;
	t->nbytes++;
	return v;
}

static void
i2c_stop(r40 *m)
{
	r40_i2c *t = &m->i2c;
	if (t->active) {
		r40_i2clog *l = &m->i2clog[m->i2clog_n++ % R40_I2CLOG];
		int n = t->nbytes < R40_I2CLOG_BYTES ? t->nbytes : R40_I2CLOG_BYTES;
		l->at = m->clk;
		l->n = t->nbytes;
		memcpy(l->b, t->buf, (size_t)n);
		ev(m, R40_EV_I2C, (t->addr | t->rd) | (t->nbytes << 8));
	}
	t->active = 0;
	t->nbytes = 0;
}

/* ------------------------------------------------------------- FX429 */

/* registers by A1 A0: 3 control (W) / status (R), 2 Tx data (W) / Rx data
 * (R), 0 / 1 syndrome low / high (R).  IRQ goes to IRQ1 (P1.6). */
#define FX_TXEN  0x01
#define FX_RXEN  0x04
#define FX_RDR   0x01	/* status: Rx data ready */
#define FX_TDR   0x08	/* Tx data ready */
#define FX_IDLE  0x10
#define FX_TIMER 0x20
#define FX_BIT   6720	/* states per bit at 1200 bd */

static void
fx_irq(r40 *m)
{
	h8532_pin(&m->chip, 1, 6, m->fx_int ? 0 : 1);
}

static void
fx_set(r40 *m, uint8_t bits)
{
	m->fx_stat |= bits;
	m->fx_int |= bits;
	fx_irq(m);
}

static uint8_t
fx429_read(r40 *m, int r)
{
	uint8_t v;
	switch (r) {
	case 3:
		v = m->fx_stat;
		m->fx_stat &= (uint8_t)~(FX_TIMER | FX_IDLE | 0xC0);
		m->fx_int = 0;
		fx_irq(m);
		return v;
	case 2:
		m->fx_stat &= (uint8_t)~(FX_RDR | 0x02);
		return m->fx_rx;
	}
	return 0;
}

static void
fx429_write(r40 *m, int r, uint8_t v)
{
	if (r == 3) {
		uint8_t old = m->fx_ctrl;
		m->fx_ctrl = v;
		ev(m, R40_EV_MODEM, 3 << 8 | v);
		if ((v & FX_TXEN) && !(old & FX_TXEN)) {
			m->fx_next = m->clk + 8 * FX_BIT;	/* a byte of preamble */
			m->fx_tx_loaded = 0;
			m->fx_sent = 0;
		}
		if (!(v & FX_TXEN)) {
			m->fx_next = 0;
			m->fx_stat &= (uint8_t)~(FX_TDR | FX_IDLE);
		}
		if (!(v & FX_RXEN))
			m->fx_stat &= (uint8_t)~(FX_RDR | 0x02 | 0xC0);
		if (v >> 4) {
			if (!m->fx_timer_next || !(old >> 4))
				m->fx_timer_next = m->clk + (uint64_t)(v >> 4) * 8 * FX_BIT;
		} else
			m->fx_timer_next = 0;
		m->fx_int &= m->fx_stat;
		fx_irq(m);
	} else if (r == 2) {
		m->fx_tx = v;
		m->fx_tx_loaded = 1;
		m->fx_stat &= (uint8_t)~FX_TDR;
		ev(m, R40_EV_MODEM, 2 << 8 | v);
	}
}

static void
fx429_run(r40 *m)
{
	if (m->fx_next && m->clk >= m->fx_next) {
		m->fx_next += 8 * FX_BIT;
		if (m->fx_tx_loaded) {
			m->fx_tx_loaded = 0;
			m->fx_sent = 1;
			fx_set(m, FX_TDR);
		} else if (m->fx_sent) {
			m->fx_sent = 0;
			fx_set(m, FX_IDLE);
		} else
			fx_set(m, FX_TDR);	/* preamble going out; ready for data */
	}
	if (m->fx_timer_next && m->clk >= m->fx_timer_next) {
		m->fx_timer_next += (uint64_t)(m->fx_ctrl >> 4) * 8 * FX_BIT;
		fx_set(m, FX_TIMER);
	}
}

void
r40_modem_rx(r40 *m, uint8_t b)
{
	if (!(m->fx_ctrl & FX_RXEN))
		return;
	m->fx_rx = b;
	fx_set(m, FX_RDR);
}

/* ------------------------------------------------------------ PCF8584 */

#define I_PIN 0x80
#define I_ESO 0x40
#define I_ES1 0x20
#define I_ES2 0x10
#define I_ENI 0x08
#define I_STA 0x04
#define I_STO 0x02
#define I_ACK 0x01
/* status */
#define I_LRB 0x08
#define I_BB  0x01

static uint64_t
i2c_byte_states(const r40 *m)
{
	static const double scl[4] = { 90e3, 45e3, 11e3, 1.5e3 };
	return (uint64_t)(R40_HZ * 9.0 / scl[m->i_s2 & 3]);
}

static void
i_int_update(r40 *m)
{
	int low = (m->i_s1 & I_ENI) && (m->i_s1 & I_ESO) && !(m->i_stat & I_PIN);
	h8532_pin(&m->chip, 7, 2, !low);
}

static void
i_begin(r40 *m)
{
	m->i_stat &= (uint8_t)~I_PIN;
	m->i_stat |= I_PIN;
	m->i_done = m->clk + i2c_byte_states(m);
	i_int_update(m);
}

static void
i_complete(r40 *m)
{
	m->i_done = 0;
	m->i_stat &= (uint8_t)~I_PIN;
	i_int_update(m);
}

static void
i_start(r40 *m)
{
	if (m->i2c.active)
		i2c_stop(m);	/* repeated start ends the previous transfer */
	m->i_stat &= (uint8_t)~I_BB;	/* busy */
	m->i_ack = i2c_write(m, m->i_s0);
	m->i_state = (m->i_s0 & 1) ? 2 : 1;
	m->i_first = m->i_state == 2;
	i_begin(m);
}

static uint8_t
pcf8584_read(r40 *m, int a0)
{
	if (a0) {
		if (!(m->i_s1 & I_ESO))
			return m->i_s1 & 0x7F;
		return (uint8_t)((m->i_stat & (I_PIN | I_BB))
		    | (m->i_done == 0 && !m->i_ack ? I_LRB : 0));
	}
	if (!(m->i_s1 & I_ESO)) {
		if (m->i_s1 & I_ES1)
			return m->i_s2;
		return (m->i_s1 & I_ES2) ? m->i_s3 : m->i_s0own;
	}
	if (m->i_s1 & I_ES2)
		return m->i_s3;
	{
		uint8_t v = m->i_rxbyte;
		if (m->i_state == 2 && !(m->i_stat & I_PIN) && !m->i_stopped) {
			/* reading S0 starts the next byte; the first read is the
			 * dummy read of the address */
			m->i_rxbyte = i2c_read(m);
			m->i_ack = 1;
			i_begin(m);
		}
		return v;
	}
}

static void
pcf8584_write(r40 *m, int a0, uint8_t v)
{
	if (a0) {
		m->i_s1 = v;
		if (v & I_PIN)
			m->i_stat = (uint8_t)((m->i_stat & I_BB) | I_PIN);
		if (!(v & I_ESO)) {
			i_int_update(m);
			return;
		}
		switch (v & (I_STA | I_STO)) {
		case I_STA:		/* START or repeated START + address */
		case I_STA | I_STO:	/* chaining */
			m->i_stopped = 0;
			if (m->i_stat & I_BB)	/* bus free: the address is in S0 */
				i_start(m);
			else			/* repeated: sent when S0 is written */
				m->i_restart = 1;
			break;
		case I_STO:
			i2c_stop(m);
			m->i_state = 0;
			m->i_stopped = 1;
			m->i_done = 0;
			m->i_stat |= I_BB | I_PIN;
			break;
		}
		i_int_update(m);
		return;
	}
	if (!(m->i_s1 & I_ESO)) {
		if (m->i_s1 & I_ES1)
			m->i_s2 = v;
		else if (m->i_s1 & I_ES2)
			m->i_s3 = v;
		else
			m->i_s0own = v;
		return;
	}
	if (m->i_s1 & I_ES2) {
		m->i_s3 = v;
		return;
	}
	m->i_s0 = v;
	if (m->i_restart) {
		m->i_restart = 0;
		i_start(m);
		return;
	}
	if (m->i_state == 1) {
		m->i_ack = i2c_write(m, v);
		i_begin(m);
	}
}

/* ------------------------------------------------------- serial bus */

/* DS1202: 8-byte clock, 24 bytes RAM; LSB first; command byte, then data */
static uint8_t
rtc_reg(r40 *m, int i)
{
	return i < 8 ? m->rtc[i] : m->rtc_ram[(i - 8) % 24];
}

static void
rtc_reg_w(r40 *m, int i, uint8_t v)
{
	if (i < 8)
		m->rtc[i] = v;
	else
		m->rtc_ram[(i - 8) % 24] = v;
}

/* index of the register a command addresses: clock 0..7, RAM 8.. */
static int
rtc_index(int cmd)
{
	int a = (cmd >> 1) & 0x1F;
	if (cmd & 0x40)
		return 8 + a;
	return a;
}

static void
rtc_clock_rise(r40 *m, int bit)
{
	if (m->rtc_n < 8) {
		m->rtc_cmd |= (uint8_t)(bit << m->rtc_n);
		if (++m->rtc_n == 8) {
			m->rtc_idx = rtc_index(m->rtc_cmd);
			m->rtc_burst = ((m->rtc_cmd >> 1) & 0x1F) == 0x1F;
			if (m->rtc_burst)
				m->rtc_idx = (m->rtc_cmd & 0x40) ? 8 : 0;
			m->rtc_bit = 0;
			m->rtc_data = 0;
		}
		return;
	}
	if (m->rtc_cmd & 1)	/* read: the CPU samples; nothing to latch */
		return;
	m->rtc_data |= (uint8_t)(bit << m->rtc_bit);
	if (++m->rtc_bit == 8) {
		rtc_reg_w(m, m->rtc_idx, m->rtc_data);
		m->rtc_idx++;
		m->rtc_bit = 0;
		m->rtc_data = 0;
	}
}

static void
rtc_clock_fall(r40 *m)
{
	if (m->rtc_n < 8 || !(m->rtc_cmd & 1))
		return;
	/* read: the next bit appears after each falling edge, the first one
	 * after the command's last */
	if (m->rtc_rdpos == 8) {
		m->rtc_idx++;
		m->rtc_rdpos = 0;
	}
	m->rtc_out = (rtc_reg(m, m->rtc_idx) >> m->rtc_rdpos) & 1;
	m->rtc_rdpos++;
}

static void
serial_update_din(r40 *m)
{
	/* P1.3 when it is an input reads the RTC's I/O; P1.4 the FX803 */
	int rtc = (m->out0 & O0_RTCCS) && m->rtc_n >= 8 && (m->rtc_cmd & 1);
	h8532_pin(&m->chip, 1, 3, rtc ? m->rtc_out : 1);
	h8532_pin(&m->chip, 1, 4, 1);
}

static void
p1_changed(r40 *m, uint8_t pins)
{
	uint8_t old = m->p1;
	m->p1 = pins;
	if ((pins & P1_SCLK) && !(old & P1_SCLK)) {
		int bit = (pins & P1_SDOUT) != 0;
		m->sbits = m->sbits << 1 | (uint32_t)bit;
		m->snbits++;
		if (m->out0 & O0_RTCCS)
			rtc_clock_rise(m, bit);
	}
	if (!(pins & P1_SCLK) && (old & P1_SCLK)) {
		if (m->out0 & O0_RTCCS)
			rtc_clock_fall(m);
	}
	serial_update_din(m);
}

static void
out0_write(r40 *m, uint8_t v)
{
	uint8_t old = m->out0;
	m->out0 = v;
	if (v != old)
		ev(m, R40_EV_OUT0, v);
	/* DAC: bits shifted while /CS low, latched as /CS rises */
	if (!(v & O0_DACCS) && (old & O0_DACCS))
		m->snbits = 0;
	if ((v & O0_DACCS) && !(old & O0_DACCS) && m->snbits >= 24) {
		int k;
		for (k = 0; k < 4; k++) {
			m->dac[k] = (uint8_t)((m->sbits >> (18 - 6 * k)) & 0x3F);
			ev(m, R40_EV_DAC, k << 8 | m->dac[k]);
		}
		m->dac_loads++;
	}
	if ((v & O0_RTCCS) && !(old & O0_RTCCS)) {
		m->rtc_n = 0;
		m->rtc_cmd = 0;
		m->rtc_rdpos = 0;
		m->rtc_out = 1;
	}
	if (!(v & O0_FXCS) && (old & O0_FXCS))
		m->snbits = 0;
	if ((v & O0_FXCS) && !(old & O0_FXCS) && m->snbits >= 8)
		ev(m, R40_EV_FX803, (int)(m->sbits >> (m->snbits - 8)) & 0xFF);
	/* TMCI clock: OUT0.6 2.016 MHz, OUT0.7 504 kHz */
	m->chip.tmci_hz = (v & O0_2MHZ) ? 2016000.0 : (v & O0_504K) ? 504000.0 : 0;
	serial_update_din(m);
}

static void
out1_write(r40 *m, uint8_t v)
{
	uint8_t old = m->out1;
	int k;
	m->out1 = v;
	if (v != old)
		ev(m, R40_EV_OUT1, v);
	if ((v & O1_TXON) != (old & O1_TXON))
		ev(m, (v & O1_TXON) ? R40_EV_TX_ON : R40_EV_TX_OFF, 0);
	for (k = 0; k < 3; k++) {
		uint8_t b = (uint8_t)(O1_SR0 << k);
		if ((v & b) && !(old & b)) {	/* 4094 strobe */
			uint8_t was = m->sreg[k];
			m->sreg[k] = (uint8_t)m->sbits;
			ev(m, R40_EV_SR, k << 8 | m->sreg[k]);
			if (k == 0 && (m->sreg[0] & 0x04) && !(was & 0x04))
				ev(m, R40_EV_POWEROFF, 0);
		}
	}
}

/* PLL: SD sampled on CLK rising, latched by the STE / SRE strobes */
static void
pll_latch(r40 *m, int which)
{
	r40_pll *p = &m->pll[which];
	uint32_t s = m->pll_bits;
	int n = m->pll_n;
	if (n >= 16 && (s & 1)) {	/* reference register: SW R13..R0 C=1 */
		p->r = (s >> 1) & 0x3FFF;
		p->sw = (s >> 15) & 1;
	} else if (n >= 19) {		/* N10..N0 A6..A0 C=0 */
		p->a = (s >> 1) & 0x7F;
		p->n = (s >> 8) & 0x7FF;
	}
	p->loads++;
	ev(m, R40_EV_SYNTH, which | n << 8);
	m->pll_n = 0;
	m->pll_bits = 0;
}

static void
p7_changed(r40 *m, uint8_t pins)
{
	uint8_t old = m->p7;
	m->p7 = pins;
	if ((pins & P7_CLK) && !(old & P7_CLK)) {
		m->pll_bits = m->pll_bits << 1 | ((pins & P7_SD) ? 1u : 0u);
		m->pll_n++;
	}
	if ((pins & P7_SRE) && !(old & P7_SRE))
		pll_latch(m, 0);
	if ((pins & P7_STE) && !(old & P7_STE))
		pll_latch(m, 1);
}

double
r40_pll_hz(const r40 *m, int which)
{
	const r40_pll *p = &m->pll[which];
	double pre = p->sw ? 64 : 128;
	if (!p->r)
		return 0;
	return (pre * p->n + p->a) * m->ref_hz / p->r;
}

/* -------------------------------------------------------------- bus */

static uint32_t
nv_index(const r40 *m, uint32_t a)
{
	/* P9.2 drives the NV SRAM's top address line: 0 selects the upper half */
	return (a & 0x3FFF) | ((m->p9 & P9_NVSEL) ? 0 : 0x4000);
}

static uint8_t
bus_read(void *ctx, uint32_t a)
{
	r40 *m = ctx;
	if (a < 0x80000)
		return m->rom[a & 0x3FFFF];
	if (a < 0x88000)
		return m->nv[nv_index(m, a)];
	if (a < 0x90000)
		return m->ram[a & 0x7FFF];
	if (a >= 0xA0000 && a < 0xA8000)
		return fx429_read(m, a & 3);
	if (a >= 0xB8000 && a < 0xC0000)
		return pcf8584_read(m, a & 1);
	return 0xFF;
}

static void
bus_write(void *ctx, uint32_t a, uint8_t v)
{
	r40 *m = ctx;
	if (a < 0x80000)
		return;
	if (a < 0x88000) {
		m->nv[nv_index(m, a)] = v;
		return;
	}
	if (a < 0x90000) {
		m->ram[a & 0x7FFF] = v;
		return;
	}
	if (a >= 0xA0000 && a < 0xA8000)
		fx429_write(m, a & 3, v);
	else if (a >= 0xA8000 && a < 0xB0000)
		out0_write(m, v);
	else if (a >= 0xB0000 && a < 0xB8000)
		out1_write(m, v);
	else if (a >= 0xB8000 && a < 0xC0000)
		pcf8584_write(m, a & 1, v);
	if (m->watch_hi && a >= m->watch_lo && a <= m->watch_hi) {
		m->watch_hit = 1;
		m->watch_addr = a;
	}
}

static void
port_out(void *ctx, int p, uint8_t pins, uint8_t ddr)
{
	r40 *m = ctx;
	(void)ddr;
	if (p == 1)
		p1_changed(m, pins);
	else if (p == 7)
		p7_changed(m, pins);
	else if (p == 9)
		m->p9 = pins;
}

static int
adc(void *ctx, int ch)
{
	r40 *m = ctx;
	return m->an[ch & 7];
}

static void
sci_tx(void *ctx, uint8_t b)
{
	r40 *m = ctx;
	ev(m, R40_EV_SCI_TX, b);
}

/* -------------------------------------------------------------- run */

uint8_t
r40_peek(r40 *m, uint32_t a)
{
	return h8532_peek(&m->chip, a);
}

void
r40_poke(r40 *m, uint32_t a, uint8_t v)
{
	h8532_poke(&m->chip, a, v);
}

void
r40_breakpoint(r40 *m, uint32_t a, int on)
{
	a &= 0xFFFFFF;
	if (!m->bp) {
		m->bp = calloc(1, 0x1000000 / 8);
		if (!m->bp)
			return;
	}
	if (on && !(m->bp[a >> 3] & (1 << (a & 7)))) {
		m->bp[a >> 3] |= (uint8_t)(1 << (a & 7));
		m->nbp++;
	} else if (!on && (m->bp[a >> 3] & (1 << (a & 7)))) {
		m->bp[a >> 3] &= (uint8_t)~(1 << (a & 7));
		m->nbp--;
	}
}

static void
periodic(r40 *m)
{
	if (m->i_done && m->clk >= m->i_done)
		i_complete(m);
	fx429_run(m);
}

int
r40_step(r40 *m)
{
	h8500 *c = &m->chip.cpu;
	uint32_t pc;
	int n;

	if (!m->powered) {
		m->clk += 8;
		return R40_STOP_OFF;
	}
	pc = h8500_pc24(c);
	if (m->nbp && !m->skip_bp && !c->sleeping && (m->bp[pc >> 3] & (1 << (pc & 7)))) {
		m->skip_bp = 1;
		return R40_STOP_BREAK;
	}
	m->skip_bp = 0;
	if (!c->sleeping) {
		m->trace[m->trace_pos] = pc;
		m->trace_pos = (m->trace_pos + 1) % R40_TRACE;
	}
	n = h8532_step(&m->chip);
	m->clk += (uint64_t)n;
	if (c->illegal) {
		c->illegal = 0;
		ev(m, R40_EV_ILLEGAL, (int)c->op_addr);
	} else if (c->last_exc == H8_VEC_ADDRERR || c->last_exc == H8_VEC_ZERODIV)
		ev(m, R40_EV_EXC, c->last_exc << 24 | (int)c->op_addr);
	periodic(m);
	if (m->watch_hit) {
		m->watch_hit = 0;
		return R40_STOP_WATCH;
	}
	return R40_STOP_TIME;
}

int
r40_run(r40 *m, double seconds)
{
	uint64_t end = m->clk + (uint64_t)(seconds * R40_HZ);
	int r;
	while (m->clk < end) {
		r = r40_step(m);
		if (r == R40_STOP_BREAK || r == R40_STOP_WATCH)
			return r;
	}
	return m->powered ? R40_STOP_TIME : R40_STOP_OFF;
}

void
r40_key(r40 *m, int row, int col, int down)
{
	if (row < 0 || row > 4 || col < 0 || col > 4)
		return;
	if (down)
		m->keys[col] |= (uint8_t)(1 << row);
	else
		m->keys[col] &= (uint8_t)~(1 << row);
	irq0_update(m);
}

void
r40_set_onoff(r40 *m, int down)
{
	m->onoff_key = down != 0;
	irq0_update(m);
}

void
r40_set_hook(r40 *m, int offhook)
{
	m->hook = offhook != 0;
	irq0_update(m);
}

void
r40_set_ptt(r40 *m, int down)
{
	h8532_pin(&m->chip, 7, 3, !down);
}

void
r40_power(r40 *m, int on)
{
	if (on && !m->powered) {
		m->powered = 1;
		m->p1 = m->p7 = 0xFF;
		m->p9 = 0;
		h8532_reset(&m->chip);
		/* P9.2 (NV RAM half) is pulled low: the firmware pushes its first
		 * return address before it makes the pin an output driving 0 */
		h8532_pin(&m->chip, 9, 2, 0);
		serial_update_din(m);
		i_int_update(m);
		irq0_update(m);
	}
	m->powered = on != 0;
}

int
r40_load_rom(r40 *m, const char *path)
{
	FILE *f = fopen(path, "rb");
	size_t n;
	if (!f)
		return -1;
	n = fread(m->rom, 1, sizeof(m->rom), f);
	fclose(f);
	return (int)n;
}

void
r40_init(r40 *m)
{
	h8532_bus b = { m, bus_read, bus_write, port_out, adc, sci_tx, NULL };
	memset(m, 0, sizeof(*m));
	memset(m->rom, 0xFF, sizeof(m->rom));
	h8532_init(&m->chip, &b, R40_HZ);
	m->chip.ftci_hz[1] = 7875.0;	/* FTCI2 */
	m->ref_hz = 12.8e6;
	m->i_stat = I_PIN | I_BB;
	m->i_s2 = 0x01;
	m->hook = 0;
	m->service_head = 0;
	m->head_cu43 = 1;
	m->eeprom[0] = 0x3A;		/* CU43PROG maintenance key (PE1BVU) */
	m->eeprom[1] = 0x01;
	m->eeprom[2] = 0xF0;
	m->an[0] = 300;			/* RSSI */
	m->an[1] = 512;			/* SQ */
	m->an[4] = 700;			/* +VB */
	m->rtc[0] = 0x00; m->rtc[1] = 0x00; m->rtc[2] = 0x12;
	m->rtc[3] = 0x05; m->rtc[4] = 0x10; m->rtc[5] = 0x01; m->rtc[6] = 0x26;
}

void
r40_free(r40 *m)
{
	free(m->bp);
	m->bp = NULL;
}
