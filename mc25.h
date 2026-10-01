/*
 * Mobira MC25 TVL/PTL logic: KL1 system unit (CDP1802 at 2.24 MHz),
 * TV1 signalling unit, CU41 control unit.
 *
 * Time is kept in CPU clock periods (2.24 MHz; a machine cycle is 8).
 * Sources: MC25TVL service manual (KL1 parts list, TV1 and CU41 texts,
 * LOCAL-mode port tables) and the OH5NXO/OH3NWQ firmware mc25.asm; see
 * notes/mc25.md.
 */
#ifndef MC25_H
#define MC25_H

#include <stdint.h>

#include "cdp1802.h"
#include "cu41.h"

#define MC25_HZ 2240000.0

/* host inputs (logical, 1 = active) */
enum {
	MC25_IN_PTT,		/* /EF1 */
	MC25_IN_OFFHOOK,	/* INP 4 bit 0 = 0: handset lifted (cradle switch
				 * released); on the hook PTT sends a 1747 Hz burst */
	MC25_IN_SQUELCH,	/* INP 4 bit 1: carrier */
	MC25_IN_LOCK,		/* bit 2, active low: the lock switch turned */
	MC25_IN_PROGRAM,	/* bit 3, active low: handset UP/DOWN button */
	MC25_IN_OFFICE,		/* bit 4, active low: the other one */
	MC25_IN_1800,		/* bit 5, active low: 1800 Hz detected */
	MC25_IN_LOCAL,		/* /EF4 service mode */
	MC25_IN_N
};

enum { MC25_STOP_TIME, MC25_STOP_BREAK, MC25_STOP_WATCH, MC25_STOP_OFF };

enum {
	MC25_EV_WDRESET = 1,
	MC25_EV_POWERON,
	MC25_EV_POWEROFF,
	MC25_EV_TX_ON,		/* OUT 3 TXON */
	MC25_EV_TX_OFF,
	MC25_EV_SYNTH,		/* divisor changed; arg = P8..P1 << 1 | P0 */
	MC25_EV_CCIR_TX,	/* OUT 5 tone code changed; arg = code (0xF off) */
	MC25_EV_CCIR_ACK,	/* arg 0 EV, 1 ARP */
	MC25_EV_DPY,		/* display byte; arg = byte */
	MC25_EV_ILLEGAL,
};

typedef struct mc25_event { uint64_t at; int type, arg; } mc25_event;

#define MC25_EVQ 1024
#define MC25_TRACE 4096
#define MC25_CCIRQ 32
#define MC25_SERQ 256

typedef struct mc25 {
	cdp1802  cpu;
	uint64_t clk;
	int      powered;

	uint8_t  rom[0x4000];	/* 0000-3FFF (8 KB images mirrored) */
	uint8_t  eerom[0x2000];	/* 4000-5FFF code EEPROM (EEPROM builds) */
	int      eerom_loaded;
	uint8_t  ram[1024];	/* 8000-83FF, mirrored to BFFF (assumed) */
	uint8_t  nv[256];	/* X2212 at C000-C0FF, 4 bits, mirrored (assumed) */

	uint8_t  out[8];	/* OUT 1, 2, 3, 5, 7 latches */
	uint8_t  in[MC25_IN_N];
	cu41     cu;

	/* 1 ms interrupt request, cleared by the acknowledge */
	uint64_t next_tick;
	unsigned ticks;
	/* watchdog: patted by every INP 4 */
	uint64_t wd_last;
	double   wd_timeout_s;

	/* TV1 CCIR decoders (0 EV, 1 ARP): queued digits, one presented
	 * at a time (detect bit high) until the firmware acknowledges */
	uint8_t  ccir_q[2][MC25_CCIRQ];
	int      ccir_n[2], ccir_head[2];
	int      ccir_cur[2];	/* presented digit, -1 none */
	uint64_t ccir_next[2];	/* next digit not before this */

	/* serial RX on /EF2 (1200 8E1, idle mark = pin low = EF2 true) */
	uint8_t  ser_q[MC25_SERQ];
	int      ser_n, ser_head;
	uint64_t ser_t0;	/* start of the character being sent */
	int      ser_busy;
	/* Q (serial TX) edges */
	uint64_t *q_t;
	uint8_t  *q_v;
	unsigned q_n, q_cap;

	mc25_event ev[MC25_EVQ];
	unsigned ev_head, ev_n;
	uint16_t trace[MC25_TRACE];
	unsigned trace_pos;
	uint8_t  bp[65536 / 8];
	int      nbp;
	uint8_t  skip_bp;
	uint16_t watch_lo, watch_hi, watch_addr;
	int      watch_hit;
} mc25;

void   mc25_init(mc25 *m);
int    mc25_load_rom(mc25 *m, const char *path);
int    mc25_load_eerom(mc25 *m, const char *path);
void   mc25_power(mc25 *m, int on);
int    mc25_run(mc25 *m, double seconds);
int    mc25_step(mc25 *m);
double mc25_time(const mc25 *m);
void   mc25_set_input(mc25 *m, int which, int v);
void   mc25_key(mc25 *m, int code);
void   mc25_ccir_rx(mc25 *m, int decoder, const uint8_t *digits, int n);
void   mc25_serial_rx(mc25 *m, const uint8_t *bytes, int n);
uint8_t mc25_peek(mc25 *m, uint16_t a);
void   mc25_poke(mc25 *m, uint16_t a, uint8_t v);
int    mc25_event_pop(mc25 *m, mc25_event *e);
void   mc25_breakpoint(mc25 *m, uint16_t a, int on);

#endif
