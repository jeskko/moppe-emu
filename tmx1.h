/*
 * Nokia TMF-1 / TMN-1 (Mobira Talkman 520 / 620) radio unit: uPD78C10
 * processor module NP5SA with 64 KB EPROM (paged 8000-BFFF), 8 KB
 * battery-backed RAM, MAS7825 modem, i8253 timer, 74259 deviation latch,
 * MB1501 / MB87006 RX and TX PLLs, MC144111 DAC and MAS7845 LFU on a
 * shared serial bus, and the MBUS to an HSN-2 / HSF-2 handset, which runs
 * its own firmware (tmx1hs.c).
 *
 * Time is kept in CPU clocks (11.0592 MHz, both units).  Sources: the
 * TMF-1XS service manual (processor module, pin table, memory map,
 * watchdog) and the hardware use of OH5NXO / OH3NWQ tmx1.asm; see
 * notes/tmx1.md.
 */
#ifndef TMX1_H
#define TMX1_H

#include <stdint.h>

#include "pit.h"
#include "tmx1hs.h"
#include "upd7810.h"

#define TMX1_HZ 11059200.0

/* A/D channels */
enum { TMX1_AN_FSKL, TMX1_AN_BCR, TMX1_AN_BTMP, TMX1_AN_RSSI,
       TMX1_AN_PFB, TMX1_AN_BATT, TMX1_AN_TEMP, TMX1_AN_TIMEOUT };

enum {
	TMX1_STOP_TIME = 0,
	TMX1_STOP_BREAK,
	TMX1_STOP_WATCH,
	TMX1_STOP_OFF,
};

enum {
	TMX1_EV_WDNMI = 1,	/* watchdog: no WDC pulse for 400 ms */
	TMX1_EV_POWEROFF,	/* PWROFF, or the watchdog 12 s after its NMI */
	TMX1_EV_POWERON,
	TMX1_EV_TX_ON,		/* TXON (PB4) set */
	TMX1_EV_TX_OFF,
	TMX1_EV_SYNTH,		/* PLL loaded; arg 0 = RX (RSEN), 1 = TX (TSEN) */
	TMX1_EV_LFU,		/* MAS7845 register written; arg = byte */
	TMX1_EV_DAC,		/* MC144111 latched */
	TMX1_EV_DEV,		/* 74259 deviation latch; arg = 3 bits */
	TMX1_EV_MODEM_TX,	/* byte written to the modem; arg = byte */
	TMX1_EV_MBUS,		/* byte seen on MBUS; arg = byte | 0x100 error | sender << 9 */
	TMX1_EV_DTMF,		/* handset DTMF / tone code */
	TMX1_EV_ILLEGAL,	/* undefined opcode; arg = PC | 0x10000 for the handset */
};

/* MBUS senders (EV_MBUS arg bits 9..11) */
#define TMX1_MB_RADIO 1
#define TMX1_MB_HS    2
#define TMX1_MB_HOST  4

typedef struct tmx1_event {
	uint64_t at;
	int      type;
	int      arg;
} tmx1_event;

typedef struct tmx1_pll {
	uint32_t r, sw, d;	/* reference divider, its extra bit, N*128+A */
	unsigned loads;
} tmx1_pll;

#define TMX1_EVQ 4096
#define TMX1_TRACE 4096
#define TMX1_HOSTQ 256

typedef struct tmx1 {
	upd7810  cpu;
	uint64_t clk;		/* radio clocks since creation */
	int      powered;
	uint8_t  rom[65536];
	uint8_t  ram[8192];

	uint8_t  an[8];
	int      ptt;		/* external /PTT (PC2 I2DA) */
	uint8_t  pa, pb, pc;	/* port pins (inputs read high) */

	/* serial bus (PA0 SCL, PA1 SDA) */
	uint64_t sbits;		/* bits clocked on SCL rising */
	uint32_t dbits;		/* DAC bits clocked on SCL falling while DADIS low */
	int      dnbits;
	tmx1_pll rx, tx;
	double   ref_hz;	/* PLL reference, 12.8 MHz */
	uint8_t  lfu[4];
	unsigned lfu_writes;
	uint8_t  dac[4];	/* in sending order: AFC, BCON, PC coarse, PC fine */
	unsigned dac_loads;
	uint8_t  dev;		/* 74259 AMU/LE outputs */

	/* MAS7825 at byte level */
	uint8_t  m_ctrl, m_stat, m_rx;
	uint64_t m_tx_done;

	/* i8253: CLK0 455 kHz IF, CLK1 921.6 kHz, CLK2 921.6 kHz (the tone mod) */
	pit      pit;
	double   if_hz;
	uint64_t pit_done0, pit_done12;
	int      out2;
	int      aud_on;
	uint64_t *aud_t;
	uint8_t  *aud_v;
	unsigned aud_n, aud_cap;

	/* watchdog: NMI 400 ms after the last WDC pulse, power off 12 s after the NMI */
	uint64_t wd_last;
	int      wd_nmi_sent;
	double   wd_nmi_s, wd_off_s;

	/* handset and MBUS */
	tmx1hs   hs;
	uint64_t hs_clk;
	int      radio_txd, hs_txd, host_txd, line;
	/* host transmitter: bytes queued for the line, 9600 8O1 */
	uint8_t  hostq[TMX1_HOSTQ];
	unsigned hq_head, hq_n;
	uint64_t host_t0;	/* start of the frame being sent, 0 idle */
	int      host_burst;	/* bytes of one packet: no free-bus wait */
	uint64_t line_t;	/* last line edge */
	uint16_t host_frame;
	/* passive receiver on the line */
	uint64_t snf_t0;
	uint64_t snf_edge[24];
	int      snf_ne;
	int      snf_from;

	tmx1_event ev[TMX1_EVQ];
	unsigned ev_head, ev_n;

	uint16_t trace[TMX1_TRACE];
	unsigned trace_pos;
	uint8_t  bp[65536 / 8];
	int      nbp;
	uint8_t  skip_bp;
	uint16_t watch_lo, watch_hi;
	int      watch_hit;
	uint16_t watch_addr;
} tmx1;

void   tmx1_init(tmx1 *m, int handset);
int    tmx1_load_rom(tmx1 *m, const char *path);
int    tmx1_load_hs_rom(tmx1 *m, const char *path);
void   tmx1_power(tmx1 *m, int on);
int    tmx1_run(tmx1 *m, double seconds);
int    tmx1_step(tmx1 *m);
double tmx1_time(const tmx1 *m);
uint8_t tmx1_peek(tmx1 *m, uint16_t a);
void   tmx1_poke(tmx1 *m, uint16_t a, uint8_t v);
void   tmx1_mbus_send(tmx1 *m, uint8_t b);
void   tmx1_modem_rx(tmx1 *m, uint8_t b);
void   tmx1_power_key(tmx1 *m, int down);
int    tmx1_event_pop(tmx1 *m, tmx1_event *e);
void   tmx1_audio_capture(tmx1 *m, int on);
void   tmx1_breakpoint(tmx1 *m, uint16_t a, int on);
double tmx1_pll_hz(const tmx1 *m, const tmx1_pll *p);

#endif
