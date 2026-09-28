/*
 * R58 logic board.  See r58.h and notes/hardware.md.
 */
#include "r58.h"

#include <stdlib.h>
#include <string.h>

/* port bases */
#define P_PIO   0x00
#define P_SIO   0x10
#define P_TMR   0x20
#define P_DA0   0x30
#define P_DA1   0x40
#define P_AD    0x50
#define P_OUT0  0x60
#define P_OUT1  0x70
#define P_OUT2  0x80
#define P_WD    0x90
#define P_MDM   0xA0
#define P_CSMEM 0xB0

#define O1_SRE   0x01
#define O1_SCE   0x02
#define O1_STE   0x04
#define O1_CLK   0x08
#define O1_SD    0x10
#define O1_RAS   0x20
#define O1_TPS   0x40
#define O1_TXOFF 0x80
#define O1_STROBES (O1_SRE | O1_SCE | O1_STE | O1_RAS | O1_TPS)

#define O2_RA14  0x01
#define O2_RA15  0x02
#define O2_RS    0x04
#define O2_SMEM  0x08		/* P8N; on P8E this latch bit is RA16 */

#define PA0_PERIOD 4096		/* xt per 1968.75 Hz cycle */

/* FX429 */
#define MDM_TXENB 0x01
#define MDM_RXENB 0x04
#define MDM_RXRDY 0x01
#define MDM_DCD   0x04
#define MDM_TXRDY 0x08
#define MDM_TXIDL 0x10
#define MDM_SYNC  0x40
#define MDM_BYTE_XT ((uint64_t)(R58_XTAL_HZ * 8 / 1200))

static void update_lines(r58 *m);

/* ---------------------------------------------------------------- events */

static void
event(r58 *m, int type, int arg)
{
	r58_event *e = &m->ev[m->ev_w % R58_EVQ];
	e->at = m->now;
	e->type = type;
	e->arg = arg;
	m->ev_w++;
	if (m->ev_w - m->ev_r > R58_EVQ)
		m->ev_r = m->ev_w - R58_EVQ;	/* drop oldest */
}

int
r58_next_event(r58 *m, r58_event *e)
{
	if (m->ev_r == m->ev_w)
		return 0;
	*e = m->ev[m->ev_r % R58_EVQ];
	m->ev_r++;
	return 1;
}

/* ---------------------------------------------------------------- memory */

static inline uint8_t *
ram_ptr(r58 *m, uint16_t a)
{
	if (m->card == R58_P8N && !(m->out2 & O2_SMEM) && a < R58_NV_BASE + R58_NV_SIZE)
		return &m->nvplane[a - R58_NV_BASE];
	return &m->ram[a - 0xC000];
}

/*
 * 0x8000-0xBFFF, both cards: RS=1 selects EPROM0 chip 0x8000 (RA14=0) or
 * 0xC000 (RA14=1) page; RS=0 selects EPROM1.  P8N: service manual p87-90;
 * P8E: schematic, EPROM0 A14 = (A15 | A14) & (!A15 | RA14) through
 * IC11/3, IC11/4, IC12/2 and IC10/4, i.e. RA14 in the window (traced by
 * the user 2026-09-28; an earlier reading had A14 forced high).  EPROM1
 * page: P8N RA15:RA14, P8E A14..A16 = OUT2 bits 0, 1, 3.  The DTMF/CTCSS
 * "multiboard" plugs into the EPROM1 socket; without an EPROM1 image the
 * socket reads as the multiboard status byte.
 */
static uint8_t
window_read(r58 *m, uint16_t a)
{
	if (m->out2 & O2_RS)
		return m->rom[0x8000 + ((m->out2 & O2_RA14) ? 0x4000 : 0) + (a & 0x3fff)];
	if (m->card == R58_P8N) {
		if (m->rom1)
			return m->rom1[(m->out2 & 3) * 0x4000 + (a & 0x3fff)];
		return m->multiboard;
	}
	if (m->rom1) {
		unsigned bank = (m->out2 & (O2_RA14 | O2_RA15)) | ((m->out2 >> 1) & 4);
		return m->rom1[bank * 0x4000 + (a & 0x3fff)];
	}
	return m->multiboard;
}

static uint8_t
mem_read(void *ctx, uint16_t a)
{
	r58 *m = ctx;

	if (a < 0x8000)
		return m->rom[a];
	if (a < 0xC000)
		return window_read(m, a);
	return *ram_ptr(m, a);
}

static void
mem_write(void *ctx, uint16_t a, uint8_t v)
{
	r58 *m = ctx;

	if (a < 0xC000)
		return;
	*ram_ptr(m, a) = v;
	if (m->wp[a] && m->stop_reason == R58_STOP_TIME) {
		m->stop_reason = R58_STOP_WATCH;
		m->stop_addr = a;
	}
}

uint8_t
r58_peek(r58 *m, uint16_t addr)
{
	return mem_read(m, addr);
}

void
r58_poke(r58 *m, uint16_t addr, uint8_t v)
{
	if (addr >= 0xC000)
		*ram_ptr(m, addr) = v;
}

/* battery-backed block: P8E keeps it in the main RAM chip, P8N in the
 * separate SMEM plane */
static uint8_t *
nv_block(r58 *m)
{
	return m->card == R58_P8N ? m->nvplane : m->ram;
}

int
r58_load_nv(r58 *m, const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return -1;
	size_t n = fread(nv_block(m), 1, R58_NV_SIZE, fp);
	fclose(fp);
	return n == R58_NV_SIZE ? 0 : -1;
}

int
r58_save_nv(r58 *m, const char *path)
{
	FILE *fp = fopen(path, "wb");
	if (!fp)
		return -1;
	size_t n = fwrite(nv_block(m), 1, R58_NV_SIZE, fp);
	fclose(fp);
	return n == R58_NV_SIZE ? 0 : -1;
}

int
r58_load_rom1(r58 *m, const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return -1;
	if (!m->rom1)
		m->rom1 = malloc(0x20000);
	memset(m->rom1, 0xff, 0x20000);
	size_t n = fread(m->rom1, 1, 0x20000, fp);
	fclose(fp);
	return n > 0 ? 0 : -1;
}

int
r58_load_rom(r58 *m, const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return -1;
	memset(m->rom, 0xff, sizeof(m->rom));
	size_t n = fread(m->rom, 1, sizeof(m->rom), fp);
	fclose(fp);
	return n > 0 ? 0 : -1;
}

/* ---------------------------------------------------------------- OUT1 */

static void
synth_latch(r58 *m, uint8_t strobe)
{
	uint64_t b = m->sbits;
	int n = m->snbits;
	r58_synth *s = &m->synth;

	m->last_frame.bits = b;
	m->last_frame.nbits = n;
	m->last_frame.strobe = strobe;
	m->last_frame.at = m->now;

	if (strobe & O1_SCE) {
		s->ctrl = b & 0xff;
		s->ctrl_loads++;
	}
	if (strobe & (O1_SRE | O1_STE)) {
		int tx = !!(strobe & O1_STE);
		if (b & 1) {
			uint32_t r = (b >> 1) & 0x3fff;
			if (tx) s->tx_r = r; else s->rx_r = r;
		} else {
			uint32_t nn = (b >> 8) & 0x3ff, a = (b >> 1) & 0x7f;
			if (tx) { s->tx_n = nn; s->tx_a = a; }
			else    { s->rx_n = nn; s->rx_a = a; }
		}
		if (tx) s->tx_loads++; else s->rx_loads++;
	}
	if (strobe & O1_RAS)
		s->ext_a = b & 0xffff;
	if (strobe & O1_TPS)
		s->ext_b = b & 0xffff;
	event(m, R58_EV_SYNTH, strobe);
	m->sbits = 0;
	m->snbits = 0;
}

static void
out1_write(r58 *m, uint8_t v)
{
	uint8_t old = m->out1;

	m->out1 = v;
	if ((v & O1_CLK) && !(old & O1_CLK)) {
		m->sbits = (m->sbits << 1) | !!(v & O1_SD);
		if (m->snbits < 64)
			m->snbits++;
	}
	uint8_t rise = v & ~old & O1_STROBES;
	if (rise)
		synth_latch(m, rise);
	if ((old ^ v) & O1_TXOFF)
		event(m, (v & O1_TXOFF) ? R58_EV_TX_OFF : R58_EV_TX_ON, 0);
}

double
r58_synth_vco_hz(const r58 *m, int tx, int prescaler, double tcxo_hz)
{
	const r58_synth *s = &m->synth;
	uint32_t r = tx ? s->tx_r : s->rx_r;
	uint32_t n = tx ? s->tx_n : s->rx_n;
	uint32_t a = tx ? s->tx_a : s->rx_a;

	if (!r)
		return -1;
	return ((double)n * prescaler + a) * tcxo_hz / r;
}

/* ---------------------------------------------------------------- FX429 */

static void
modem_irq(r58 *m)
{
	/* each modem event produces an edge on SIO A DCD (inferred wiring) */
	m->modem_irq ^= 1;
	sio_set_status(&m->sio, 0, SIO_RR0_DCD, m->modem_irq ? SIO_RR0_DCD : 0);
}

static void
modem_tick(r58 *m)
{
	if (m->mdm_txbusy && m->now >= m->mdm_txready_at) {
		m->mdm_txbusy = 0;
		m->mdm_status |= MDM_TXRDY | MDM_TXIDL;
	}
	if (m->mdm_rx_state && m->now >= m->mdm_rx_next && (m->mdm_ctrl & MDM_RXENB)) {
		if (m->mdm_rx_state == 1) {
			m->mdm_status |= MDM_SYNC | MDM_DCD;
			m->mdm_rx_state = 2;
			modem_irq(m);
		} else if (m->mdm_rxpos < m->mdm_rxn) {
			m->mdm_status |= MDM_RXRDY;
			m->mdm_status &= ~MDM_SYNC;
			modem_irq(m);
		} else {
			m->mdm_rx_state = 0;
			m->mdm_status &= ~MDM_DCD;
		}
		m->mdm_rx_next = m->now + MDM_BYTE_XT;
	}
}

void
r58_modem_rx(r58 *m, const uint8_t *buf, int n)
{
	if (n > (int)sizeof(m->mdm_rxq))
		n = sizeof(m->mdm_rxq);
	memcpy(m->mdm_rxq, buf, n);
	m->mdm_rxn = n;
	m->mdm_rxpos = 0;
	m->mdm_rx_state = 1;
	m->mdm_rx_next = m->now;
}

static uint8_t
modem_read(r58 *m, int reg)
{
	if (reg == 3) {
		uint8_t s = m->mdm_status;
		/* SYNC is reported once */
		m->mdm_status &= ~MDM_SYNC;
		return s;
	}
	if (reg == 2) {
		uint8_t v = 0xff;
		if (m->mdm_status & MDM_RXRDY) {
			v = m->mdm_rxq[m->mdm_rxpos++];
			m->mdm_status &= ~MDM_RXRDY;
		}
		return v;
	}
	return 0xff;
}

static void
modem_write(r58 *m, int reg, uint8_t v)
{
	if (reg == 3) {
		m->mdm_ctrl = v;
		if (v & MDM_TXENB) {
			if (!m->mdm_txbusy)
				m->mdm_status |= MDM_TXRDY | MDM_TXIDL;
		} else
			m->mdm_status &= ~(MDM_TXRDY | MDM_TXIDL);
		if (!(v & MDM_RXENB))
			m->mdm_status &= ~(MDM_RXRDY | MDM_SYNC);
	} else if (reg == 2 && (m->mdm_ctrl & MDM_TXENB)) {
		event(m, R58_EV_MODEM_TX, v);
		m->mdm_status &= ~(MDM_TXRDY | MDM_TXIDL);
		m->mdm_txbusy = 1;
		m->mdm_txready_at = m->now + MDM_BYTE_XT;
	}
}

/* ---------------------------------------------------------------- I/O */

static void pit_out(void *ctx, int counter, int level, unsigned off);

static uint8_t
io_read(void *ctx, uint16_t port)
{
	r58 *m = ctx;
	uint8_t p = port & 0xff;
	int reg = p & 3;

	switch (p & 0xf0) {
	case P_PIO:
		if (reg < 2)
			return pio_read_data(&m->pio, reg);
		return 0xff;
	case P_SIO:
		switch (reg) {
		case 0: return sio_read_data(&m->sio, 0);
		case 1: return sio_read_data(&m->sio, 1);
		case 2: return sio_read_ctrl(&m->sio, 0);
		default: return sio_read_ctrl(&m->sio, 1);
		}
	case P_TMR:
		return pit_read(&m->pit, reg);
	case P_AD:
		return m->adc_result;
	case P_MDM:
		return modem_read(m, reg);
	}
	return 0xff;
}

static void
io_write(void *ctx, uint16_t port, uint8_t v)
{
	r58 *m = ctx;
	uint8_t p = port & 0xff;
	int reg = p & 3;

	switch (p & 0xf0) {
	case P_PIO:
		if (reg < 2)
			pio_write_data(&m->pio, reg, v);
		else
			pio_write_ctrl(&m->pio, reg - 2, v);
		update_lines(m);
		break;
	case P_SIO:
		switch (reg) {
		case 0: sio_write_data(&m->sio, 0, v); break;
		case 1: sio_write_data(&m->sio, 1, v); break;
		case 2: sio_write_ctrl(&m->sio, 0, v); break;
		default: sio_write_ctrl(&m->sio, 1, v); break;
		}
		break;
	case P_TMR:
		pit_write(&m->pit, reg, v);
		break;
	case P_DA0:
		m->da_rfc = v;
		break;
	case P_DA1:
		m->da_txpwr = v;
		break;
	case P_AD:
		/* start conversion on channel; result ready well before the
		 * firmware reads it (~2 ms later) */
		m->adc_result = m->adc[p & 7];
		break;
	case P_OUT0:
		m->out0 = v;
		break;
	case P_OUT1:
		out1_write(m, v);
		break;
	case P_OUT2:
		m->out2 = v;
		if (m->cu == R58_CU53AN)
			cu53an_out2(&m->cu53, v);
		else
			cu58af_set_scl(&m->cu58, !!(v & 0x40));
		update_lines(m);
		break;
	case P_WD:
		m->wd_last = m->now;
		break;
	case P_MDM:
		modem_write(m, reg, v);
		break;
	case P_CSMEM:
		m->csmem = v;
		break;
	}
}

static uint8_t
int_ack(void *ctx)
{
	r58 *m = ctx;
	return daisy_ack(&m->irq);
}

static void
reti(void *ctx)
{
	r58 *m = ctx;
	daisy_reti(&m->irq);
}

/* recompute every externally driven line after a state change */
static void
update_lines(r58 *m)
{
	uint8_t pb_drv = pio_driven(&m->pio, 1);
	uint8_t pb_out = m->pio.p[1].out;
	int dcu;

	if (m->cu == R58_CU58AF) {
		int master = (pb_drv & 0x08) ? !!(pb_out & 0x08) : 1;
		cu58af_set_sda(&m->cu58, master);
		dcu = cu58af_sda(&m->cu58);
		/* /INT low -> RR0 CTS = 1 */
		sio_set_status(&m->sio, 0, SIO_RR0_CTS,
		               cu58af_int(&m->cu58) ? 0 : SIO_RR0_CTS);
	} else {
		dcu = cu53an_dcu(&m->cu53);
		/* DA high -> RR0 CTS = 0 */
		sio_set_status(&m->sio, 0, SIO_RR0_CTS,
		               cu53an_da(&m->cu53) ? 0 : SIO_RR0_CTS);
	}

	/* PIO B inputs; open-collector pins read low when driven low by us */
	uint8_t pb = 0x01 | 0x10 | 0x40 | 0x80;
	if (m->exin1) pb |= 0x02;
	if (m->exin2) pb |= 0x04;
	if (dcu)      pb |= 0x08;
	if (m->tmr0)  pb |= 0x20;
	pio_set_input(&m->pio, 1, pb);

	/* power relay: PB7 driven high drops it */
	if ((pb_drv & 0x80) && (pb_out & 0x80) && m->powered) {
		m->powered = 0;
		event(m, R58_EV_POWEROFF, 0);
	}

	/* SIO B status: /PTT on CTS, /LOCAL on SYNC */
	sio_set_status(&m->sio, 1, SIO_RR0_CTS | SIO_RR0_SYNC,
	               (m->ptt ? SIO_RR0_CTS : 0) | (m->local ? SIO_RR0_SYNC : 0));
}

static void
set_pa(r58 *m, int pa0)
{
	uint8_t pa = (m->ccir_nibble & 0x0f) << 4;
	if (pa0)
		pa |= 0x01;
	if ((m->offhook ? m->hook_offhook_level : !m->hook_offhook_level))
		pa |= 0x02;
	pa |= 0x04;			/* WDR: unused, idle high */
	if (!m->power_on)
		pa |= 0x08;		/* PA_PWR = 1: switch off */
	pio_set_input(&m->pio, 0, pa);
}

/* ---------------------------------------------------------------- serial */

static void
sio_tx(void *ctx, int chan, uint8_t v)
{
	r58 *m = ctx;
	event(m, chan ? R58_EV_MBUS_TX : R58_EV_GPS_TX, v);
}

void
r58_serial_rx(r58 *m, int chan, const uint8_t *buf, int n)
{
	for (int i = 0; i < n; i++) {
		int w = m->rxq_w[chan];
		int nw = (w + 1) % R58_TXQ;
		if (nw == m->rxq_r[chan])
			break;			/* host queue full */
		m->rxq[chan][w] = buf[i];
		m->rxq_w[chan] = nw;
	}
}

static void
serial_tick(r58 *m)
{
	double t = m->now / R58_XTAL_HZ;

	sio_tick(&m->sio, t);
	for (int c = 0; c < 2; c++) {
		if (m->rxq_r[c] == m->rxq_w[c] || m->now < m->rx_next[c])
			continue;
		double baud = m->sio.ch[c].baud;
		if (baud <= 0 || !sio_rx_ready(&m->sio, c))
			continue;
		sio_rx_byte(&m->sio, c, m->rxq[c][m->rxq_r[c]]);
		m->rxq_r[c] = (m->rxq_r[c] + 1) % R58_TXQ;
		m->rx_next[c] = m->now + (uint64_t)(10.0 / baud * R58_XTAL_HZ);
	}
}

/* ---------------------------------------------------------------- timers */

static void
pit_out(void *ctx, int counter, int level, unsigned off)
{
	r58 *m = ctx;

	if (counter != 1 || !m->aud_cap || m->aud_n >= m->aud_cap)
		return;
	/* called from advance() before `now` moves: offset is in CLK1
	 * periods (2 xt) from the start of the batch */
	m->aud_t[m->aud_n] = m->now + m->pit01_rem + 2 * (uint64_t)off;
	m->aud_v[m->aud_n] = level;
	m->aud_n++;
}

void
r58_audio_capture(r58 *m, unsigned capacity)
{
	free(m->aud_t);
	free(m->aud_v);
	m->aud_t = 0;
	m->aud_v = 0;
	m->aud_n = 0;
	m->aud_cap = 0;
	if (capacity) {
		m->aud_t = malloc(capacity * sizeof(*m->aud_t));
		m->aud_v = malloc(capacity);
		if (m->aud_t && m->aud_v)
			m->aud_cap = capacity;
	}
}

unsigned
r58_audio_take(r58 *m, uint64_t *t, uint8_t *v, unsigned max)
{
	unsigned n = m->aud_n < max ? m->aud_n : max;
	memcpy(t, m->aud_t, n * sizeof(*t));
	memcpy(v, m->aud_v, n);
	memmove(m->aud_t, m->aud_t + n, (m->aud_n - n) * sizeof(*t));
	memmove(m->aud_v, m->aud_v + n, m->aud_n - n);
	m->aud_n -= n;
	return n;
}

/* ---------------------------------------------------------------- reset */

static void
hw_reset(r58 *m)
{
	z80_reset(&m->cpu);
	pio_reset(&m->pio);
	sio_reset(&m->sio);
	daisy_reset(&m->irq);
	/* output latches are cleared by /RESET */
	/* (TXOFF modelled as released-safe: firmware rewrites it at once) */
	m->out0 = 0;
	m->out1 = O1_TXOFF;
	m->out2 = 0;
	m->csmem = 0;
	m->wd_last = m->now;
	m->sbits = 0;
	m->snbits = 0;
	m->mdm_ctrl = 0;
	m->mdm_status = 0;
	m->mdm_txbusy = 0;
	if (m->cu == R58_CU53AN)
		cu53an_out2(&m->cu53, 0);
	else
		cu58af_set_scl(&m->cu58, 0);
	set_pa(m, (m->now % PA0_PERIOD) < PA0_PERIOD / 2);
	update_lines(m);
}

void
r58_reset(r58 *m)
{
	hw_reset(m);
}

void
r58_init(r58 *m, int card, int cu)
{
	memset(m, 0, sizeof(*m));
	m->card = card;
	m->cu = cu;
	m->wd_timeout_s = 0.52;	/* 74HC4040 at 1968.75 Hz (service manual p86) */
	m->m1_wait = card == R58_P8E ? 1 : 0;
	m->hook_offhook_level = 0;
	m->power_on = 1;
	m->powered = 1;
	m->ccir_nibble = 0x0f;
	m->exin1 = 1;
	m->exin2 = 0;		/* /IGN low: ignition on, no auto power-off */
	m->adc[AD_RSSI] = 0x20;
	m->adc[AD_SQL] = 0x00;
	m->adc[AD_BATT] = 0xe2;	/* 13.8 V */
	m->adc[AD_TP4] = 0x80;
	m->multiboard = 0x00;
	memset(m->rom, 0xff, sizeof(m->rom));

	m->cpu.ctx = m;
	m->cpu.read = mem_read;
	m->cpu.write = mem_write;
	m->cpu.in = io_read;
	m->cpu.out = io_write;
	m->cpu.int_ack = int_ack;
	m->cpu.reti = reti;

	pio_init(&m->pio);
	sio_init(&m->sio);
	m->sio.clock_hz[0] = m->sio.clock_hz[1] = 153600;
	for (int c = 0; c < 2; c++) {
		m->sio.ch[c].tx_byte = sio_tx;
		m->sio.ch[c].ctx = m;
	}
	m->pit.out_changed = pit_out;
	m->pit.ctx = m;
	pit_init(&m->pit);
	cu53an_init(&m->cu53);
	cu58af_init(&m->cu58);

	/* daisy chain: PIO first (IEO -> SIO IEI on the P8E block diagram) */
	daisy_add(&m->irq, &m->pio.p[0].irq);
	daisy_add(&m->irq, &m->pio.p[1].irq);
	sio_add_irqs(&m->sio, &m->irq);

	hw_reset(m);
}

void
r58_power(r58 *m, int on)
{
	if (on == m->power_on)
		return;
	m->power_on = on;
	set_pa(m, (m->now % PA0_PERIOD) < PA0_PERIOD / 2);
	if (on) {
		if (!m->powered) {
			m->powered = 1;
			event(m, R58_EV_POWERON, 0);
			hw_reset(m);
		}
	} else if (m->powered) {
		/* switch-off raises NMI: firmware saves NV and powers down */
		m->cpu.nmi_pending = 1;
		event(m, R58_EV_NMI, 0);
	}
}

/* ---------------------------------------------------------------- inputs */

void r58_set_adc(r58 *m, int ch, uint8_t v) { m->adc[ch & 7] = v; }
void r58_set_multiboard(r58 *m, uint8_t v) { m->multiboard = v; }

void
r58_set_ptt(r58 *m, int pressed)
{
	m->ptt = !!pressed;
	if (m->cu == R58_CU58AF)
		m->cu58.ptt = m->ptt;	/* handset tangent follows too */
	update_lines(m);
}

void
r58_set_local(r58 *m, int grounded)
{
	m->local = !!grounded;
	update_lines(m);
}

/* /IGN (PB2 = EXIN2): grounded while the ignition is on; open (1) starts
 * the firmware's auto power-off hour count (once_per_hour). */
void
r58_set_ign(r58 *m, int on)
{
	m->exin2 = !on;
	update_lines(m);
}

void
r58_set_hook(r58 *m, int offhook)
{
	int changed = m->offhook != !!offhook;
	m->offhook = !!offhook;
	m->cu58.offhook = m->offhook;
	set_pa(m, (m->now % PA0_PERIOD) < PA0_PERIOD / 2);
	if (changed)
		modem_irq(m);	/* DCDA is shared: hook, 8254 OUT2, FX429 */
	update_lines(m);
}

void
r58_set_ccir(r58 *m, int nibble)
{
	m->ccir_nibble = nibble & 0x0f;
}

int
r58_key(r58 *m, int key)
{
	int ok = 1;

	if (m->cu == R58_CU58AF) {
		ok = cu58af_set_key(&m->cu58, key);
	} else {
		static const struct { char k; int code; } map[] = {
			{ '1', CU53_KEY_1 }, { '2', CU53_KEY_2 }, { '3', CU53_KEY_3 },
			{ '4', CU53_KEY_4 }, { '5', CU53_KEY_5 }, { '6', CU53_KEY_6 },
			{ '7', CU53_KEY_7 }, { '8', CU53_KEY_8 }, { '9', CU53_KEY_9 },
			{ '0', CU53_KEY_0 }, { '*', CU53_KEY_STAR }, { '#', CU53_KEY_HASH },
			{ 'C', CU53_KEY_CL }, { 'S', CU53_KEY_STO }, { 'R', CU53_KEY_RCL },
			{ 'E', CU53_KEY_ENT }, { 'B', CU53_KEY_SHIFT },
			{ '+', CU53_KEY_PLUS }, { '-', CU53_KEY_MINUS },
		};
		int code = CU53_KEY_NONE;
		if (key) {
			ok = 0;
			for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
				if (map[i].k == key) {
					code = map[i].code;
					ok = 1;
				}
		}
		if (ok)
			cu53an_set_key(&m->cu53, code);
	}
	update_lines(m);
	return ok;
}

/* ---------------------------------------------------------------- run */

double
r58_time(const r58 *m)
{
	return m->now / R58_XTAL_HZ;
}

static void
advance(r58 *m, uint64_t dt)
{
	/* 1968.75 Hz square on PA0 and 8254 CLK2 */
	uint64_t ph0 = m->now % PA0_PERIOD;
	uint64_t ph1 = ph0 + dt;
	if (ph0 < PA0_PERIOD / 2 && ph1 >= PA0_PERIOD / 2) {
		set_pa(m, 0);				/* falling edge */
		pit_clock(&m->pit, 2, 1);		/* 8254 counts on falling CLK */
	}
	if (ph1 >= PA0_PERIOD)
		set_pa(m, 1);				/* rising edge */

	/* CLK0/CLK1 = 4.032 MHz = xt / 2 */
	m->pit01_rem += dt;
	unsigned clocks = (unsigned)(m->pit01_rem >> 1);
	m->pit01_rem &= 1;
	if (clocks) {
		pit_clock(&m->pit, 0, clocks);
		pit_clock(&m->pit, 1, clocks);
	}
	m->now += dt;
}

/* execute exactly one instruction (or interrupt acceptance) */
void
r58_step(r58 *m)
{
	int xt_per_t = m->card == R58_P8E ? 1 : 2;

	if (!m->powered)
		return;
	m->trace[m->trace_pos++ % R58_TRACE] = m->cpu.pc;
	m->cpu.int_line = daisy_int_line(&m->irq);
	unsigned t = z80_step(&m->cpu) + m->cpu.m1 * m->m1_wait;
	m->cpu_cycles += t;
	m->instructions++;
	advance(m, (uint64_t)t * xt_per_t);
	serial_tick(m);
	modem_tick(m);
}

int
r58_run(r58 *m, double seconds)
{
	/* round up: any positive request makes progress (callers pacing to
	 * a wall clock ask for tiny remainders) */
	double want = seconds * R58_XTAL_HZ;
	uint64_t end = m->now + (want > 0 ? (uint64_t)want + ((double)(uint64_t)want < want) : 0);
	int xt_per_t = m->card == R58_P8E ? 1 : 2;
	int m1_wait = m->m1_wait;
	uint64_t wd_xt = (uint64_t)(m->wd_timeout_s * R58_XTAL_HZ);
	uint64_t next_slow = m->now;

	m->stop_reason = R58_STOP_TIME;
	while (m->now < end) {
		if (!m->powered) {
			m->now = end;
			return R58_STOP_HALTED_OFF;
		}
		uint16_t pc = m->cpu.pc;
		/* a run resumed at a breakpoint executes that instruction; the
		 * skip is for the first instruction only (not for the next hit
		 * elsewhere, nor after the PC was moved) */
		if (m->bp[pc] && !m->cpu.halted &&
		    !(m->bp_skip && pc == m->stop_addr)) {
			m->stop_reason = R58_STOP_BREAK;
			m->stop_addr = pc;
			m->bp_skip = 1;
			return R58_STOP_BREAK;
		}
		m->bp_skip = 0;
		m->trace[m->trace_pos++ % R58_TRACE] = pc;

		m->cpu.int_line = daisy_int_line(&m->irq);
		unsigned t = z80_step(&m->cpu);
		t += m->cpu.m1 * m1_wait;
		m->cpu_cycles += t;
		m->instructions++;
		advance(m, (uint64_t)t * xt_per_t);

		/* slower housekeeping every ~20 us */
		if (m->now >= next_slow) {
			next_slow = m->now + 160;
			serial_tick(m);
			modem_tick(m);
			if (m->now - m->wd_last > wd_xt) {
				event(m, R58_EV_WDRESET, m->cpu.pc);
				hw_reset(m);
				if (!m->power_on) {
					/* switched off: firmware would loop in
					 * powerdown; the relay is released */
					m->powered = 0;
					event(m, R58_EV_POWEROFF, 1);
				}
			}
		}
		if (m->stop_reason != R58_STOP_TIME)
			return m->stop_reason;
	}
	return R58_STOP_TIME;
}

/* ---------------------------------------------------------------- display */

/* cu53an_font (hardware 7-seg encoding: bit0 f, 1 e, 2 c, 3 b, 4 a, 5 g, 6 d)
 * used for reverse lookup; preference order decides ambiguous glyphs */
static const char cu53_pref[] =
	"0123456789 -_AbCdEFGHIJLnoPqrStUY=\"'?:cehijlu[]^";

static uint8_t
cu53_glyph_of(char ch)
{
	static const uint8_t font[128] = {
		['0'] = 0x5f, ['1'] = 0x0c, ['2'] = 0x7a, ['3'] = 0x7c, ['4'] = 0x2d,
		['5'] = 0x75, ['6'] = 0x77, ['7'] = 0x1c, ['8'] = 0x7f, ['9'] = 0x7d,
		[' '] = 0x00, ['-'] = 0x20, ['_'] = 0x40, ['A'] = 0x3f, ['b'] = 0x67,
		['C'] = 0x53, ['d'] = 0x6e, ['E'] = 0x73, ['F'] = 0x33, ['G'] = 0x57,
		['H'] = 0x2f, ['I'] = 0x0c, ['J'] = 0x4e, ['L'] = 0x43, ['n'] = 0x26,
		['o'] = 0x66, ['P'] = 0x3b, ['q'] = 0x3d, ['r'] = 0x22, ['S'] = 0x75,
		['t'] = 0x63, ['U'] = 0x4f, ['Y'] = 0x6d, ['='] = 0x60, ['"'] = 0x09,
		['\''] = 0x08, ['?'] = 0x3a, [':'] = 0x50, ['c'] = 0x62,
		['e'] = 0x62, ['h'] = 0x27, ['i'] = 0x04, ['j'] = 0x44, ['l'] = 0x42,
		['u'] = 0x46, ['['] = 0x53, [']'] = 0x5c, ['^'] = 0x10,
	};
	return font[(unsigned char)ch];
}

static char
cu53_char_of(uint8_t g)
{
	for (const char *p = cu53_pref; *p; p++)
		if (cu53_glyph_of(*p) == g)
			return *p;
	return '?';
}

/* digit base positions: {f,e,c,b} at base..base+3, {a,g,d} at base+64.. */
static const uint8_t cu53_upper[6] = { 32, 36, 40, 44, 48, 52 };	/* u5..u0 */
static const uint8_t cu53_lower[10] = { 56, 60, 0, 4, 8, 12, 16, 20, 24, 28 }; /* d9..d0 */

static uint8_t
cu53_digit_glyph(const cu53an *c, int base)
{
	uint8_t g = 0;
	for (int i = 0; i < 4; i++)
		g |= cu53an_segment(c, base + i) << i;
	for (int i = 0; i < 3; i++)
		g |= cu53an_segment(c, base + 64 + i) << (4 + i);
	return g;
}

int
r58_display_text(const r58 *m, char *upper, int ulen, char *lower, int llen)
{
	int icons = 0;

	if (m->cu == R58_CU53AN) {
		int i;
		for (i = 0; i < 6 && i < ulen - 1; i++)
			upper[i] = cu53_char_of(cu53_digit_glyph(&m->cu53, cu53_upper[i]));
		upper[i] = 0;
		for (i = 0; i < 10 && i < llen - 1; i++)
			lower[i] = cu53_char_of(cu53_digit_glyph(&m->cu53, cu53_lower[i]));
		lower[i] = 0;
		/* special segments: positions 0x43..0x7f step 4 */
		for (int k = 0; k < 16; k++)
			if (cu53an_segment(&m->cu53, 0x43 + 4 * k))
				icons |= 1 << k;
		return icons;
	}
	/* CU58AF: 14-segment cells; words 2..9 upper, 11..19 lower */
	static const struct { char c; uint16_t w; } f[] = {
		{ '0', 0x9a59 }, { '1', 0x4002 }, { '2', 0x1c38 }, { '3', 0x1e28 },
		{ '4', 0x0e60 }, { '5', 0x1668 }, { '6', 0x1678 }, { '7', 0x0a08 },
		{ '8', 0x1e78 }, { '9', 0x1e68 }, { ' ', 0x0000 }, { 'A', 0x0e78 },
		{ 'B', 0xb078 }, { 'C', 0x1058 }, { 'D', 0x5a0a }, { 'E', 0x1478 },
		{ 'F', 0x0478 }, { 'G', 0x1658 }, { 'H', 0x0ff0 }, { 'I', 0x518a },
		{ 'J', 0x1b90 }, { 'K', 0xa1f0 }, { 'L', 0x11d0 }, { 'M', 0x8bd4 },
		{ 'N', 0x2bd4 }, { 'O', 0x1bd8 }, { 'P', 0x0df8 }, { 'Q', 0x3bd8 },
		{ 'R', 0x2df8 }, { 'S', 0x318c }, { 'T', 0x418a }, { 'U', 0x1bd0 },
		{ 'V', 0x81d1 }, { 'W', 0x2bd1 }, { 'X', 0xa185 }, { 'Y', 0x8186 },
		{ 'Z', 0x9189 }, { '-', 0x05a0 }, { '+', 0x45a2 }, { '*', 0xe5a7 },
		{ '/', 0x8181 }, { '.', 0x0190 }, { ':', 0x01d0 }, { '=', 0x15a0 },
		{ '>', 0x0185 }, { '<', 0xa180 }, { '_', 0x1180 }, { '#', 0x17b0 },
		{ '?', 0x818a }, { '!', 0x918c }, { '\'', 0x01c0 }, { '"', 0x41c0 },
		{ '$', 0x57ea }, { '%', 0x83c1 }, { '&', 0xb18d }, { ',', 0x0181 },
		{ '@', 0x5dd8 }, { '|', 0x4182 },
	};
	int i;
	for (i = 0; i < 8 && i < ulen - 1; i++) {
		uint16_t w = cu58af_cell(&m->cu58, 2 + i);
		char ch = '?';
		for (unsigned k = 0; k < sizeof(f) / sizeof(f[0]); k++)
			if (f[k].w == w) { ch = f[k].c; break; }
		upper[i] = ch;
	}
	upper[i] = 0;
	for (i = 0; i < 9 && i < llen - 1; i++) {
		uint16_t w = cu58af_cell(&m->cu58, 11 + i);
		char ch = '?';
		for (unsigned k = 0; k < sizeof(f) / sizeof(f[0]); k++)
			if (f[k].w == w) { ch = f[k].c; break; }
		lower[i] = ch;
	}
	lower[i] = 0;
	icons = cu58af_cell(&m->cu58, 1) >> 8;
	return icons;
}
