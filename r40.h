/*
 * Nokia R40 series (RC40 / RD40) logic board L100: H8/532 in mode 3 at
 * φ = 8.064 MHz, 256 KB EPROM (27C020) at 00000, battery-backed SRAM at
 * 80000 (16 KB window; P9.2 selects its half), SRAM 32 KB at 88000, FX429
 * modem at A0000, OUT0 / OUT1 latches (74HC374) at A8000 / B0000, PCF8584
 * I2C controller at B8000; security PROM at C0000 not mounted.
 *
 * Time is kept in states of φ.  Sources: Nokia logic section functional
 * description (1993), OH5NXO's R40 bring-up notes (PINS) and the original
 * firmware's use of the hardware; see notes/r40.md.
 */
#ifndef R40_H
#define R40_H

#include <stdint.h>

#include "h8532.h"

#define R40_HZ 8064000.0

enum {
	R40_STOP_TIME = 0,
	R40_STOP_BREAK,
	R40_STOP_WATCH,
	R40_STOP_OFF,
};

enum {
	R40_EV_ILLEGAL = 1,	/* invalid instruction exception; arg = PC */
	R40_EV_EXC,		/* address error / zero divide; arg = vector << 24 | PC */
	R40_EV_OUT0,		/* OUT0 latch written; arg = value */
	R40_EV_OUT1,		/* OUT1 latch written; arg = value */
	R40_EV_SR,		/* 4094 shift register strobed; arg = n << 8 | byte */
	R40_EV_DAC,		/* MC144111 loaded; arg = channel << 8 | value */
	R40_EV_SYNTH,		/* PLL loaded; arg 0 = RX (SRE), 1 = TX (STE) */
	R40_EV_I2C,		/* I2C transfer done; arg = address byte | bytes << 8 */
	R40_EV_MODEM,		/* FX429 register written; arg = reg << 8 | value */
	R40_EV_SCI_TX,		/* SCI byte sent; arg = byte */
	R40_EV_FX803,		/* FX803 command / data byte; arg = byte */
	R40_EV_TX_ON,
	R40_EV_TX_OFF,
	R40_EV_DTMF,		/* PCD3312 (I2C 0x48) byte; arg = byte */
	R40_EV_POWEROFF,	/* power-off bit of the audio shift register set */
};

typedef struct r40_event {
	uint64_t at;
	int      type;
	int      arg;
} r40_event;

/* Fujitsu dual-modulus PLL as loaded by the firmware */
typedef struct r40_pll {
	uint32_t r, sw, n, a;
	unsigned loads;
	uint32_t shift;		/* bits clocked since the last strobe */
	int      nbits;
} r40_pll;

/* I2C slave on the control-head bus */
#define R40_I2C_MAX 512
typedef struct r40_i2c {
	/* current transfer */
	int      active, addr, nbytes, rd;
	uint8_t  buf[R40_I2C_MAX];
} r40_i2c;

/* completed I2C transfers, newest last */
#define R40_I2CLOG 4096
#define R40_I2CLOG_BYTES 48
typedef struct r40_i2clog {
	uint64_t at;
	int      n;		/* bytes including the address */
	uint8_t  b[R40_I2CLOG_BYTES];
} r40_i2clog;

#define R40_EVQ 4096
#define R40_TRACE 4096
#define R40_COV   0x40000	/* ROM addresses covered by the coverage map */

typedef struct r40 {
	h8532    chip;
	uint64_t clk;		/* states since creation */
	int      powered;

	uint8_t  rom[0x40000];
	uint8_t  nv[0x8000];
	uint8_t  ram[0x8000];
	uint8_t  out0, out1;

	/* serial bus: P1.2 CLK, P1.3 DATA out, P1.4 DATA in */
	uint8_t  p1, p7, p9;
	uint32_t sbits;		/* bits shifted on CLK rising, MSB-first */
	int      snbits;
	uint8_t  sreg[3];	/* 4094 outputs */
	uint8_t  dac[4];
	unsigned dac_loads;

	r40_pll  pll[2];	/* 0 RX, 1 TX */
	uint32_t pll_bits;	/* SD bits clocked since the last strobe */
	int      pll_n;
	double   ref_hz;	/* PLL reference */

	/* DS1202 on the serial bus (CS = OUT0.1) */
	uint8_t  rtc[8], rtc_ram[24];
	uint8_t  rtc_cmd, rtc_data, rtc_out;
	int      rtc_n, rtc_idx, rtc_burst, rtc_bit, rtc_rdpos;

	/* FX429 FFSK modem (1200 bd), byte level */
	uint8_t  fx_ctrl, fx_stat, fx_int, fx_rx, fx_tx;
	int      fx_tx_loaded, fx_sent;
	uint64_t fx_next, fx_timer_next;

	/* analogue inputs, 0..1023 */
	int      an[8];

	/* PCF8584 */
	uint8_t  i_s0, i_s0own, i_s1, i_s2, i_s3, i_stat;
	int      i_state;	/* 0 idle, 1 master transmitter, 2 master receiver */
	uint64_t i_done;	/* time the byte in progress completes, 0 none */
	int      i_first;	/* next S0 read is the dummy read after the address */
	int      i_ack, i_stopped, i_restart;
	uint8_t  i_rxbyte;
	r40_i2c  i2c;
	r40_i2clog i2clog[R40_I2CLOG];
	unsigned i2clog_n;	/* total logged */

	/* control head (CU43): I2C devices */
	uint8_t  pcf[3];	/* PCF8574 0x40, 0x42, 0x44: written latches */
	uint8_t  keys[5];	/* pressed keys: row bitmasks per column */
	uint8_t  pcf190_seen;	/* IC190 inputs at its last read (its /INT) */
	int      hook, onoff_key;
	uint8_t  lcd[4][4][40];	/* driver, bank (row group), column byte */
	int      lcd_dev, lcd_bank, lcd_x, lcd_mode, lcd_cmd, lcd_mode_set, lcd_start;
	unsigned lcd_writes;
	uint8_t  eeprom[256];
	int      ee_ptr;
	int      service_head;	/* CU43PROG: 24C02 at A0 */
	int      head_cu43;	/* IC200 P6 strap */

	r40_event ev[R40_EVQ];
	unsigned ev_head, ev_n;

	uint32_t trace[R40_TRACE];
	unsigned trace_pos;
	uint8_t  *cov;		/* R40_COV flags, allocated on demand: 1 executed,
				 * 2 entered other than from the previous
				 * instruction (jump, call, return, interrupt),
				 * 4 call target, 8 exception entry */
	uint32_t cov_next;
	uint8_t  *bp;		/* 16 MB / 8 bitmap, allocated on demand */
	int      nbp;
	uint8_t  skip_bp;
	uint32_t watch_lo, watch_hi;
	int      watch_hit;
	uint32_t watch_addr;
} r40;

void    r40_init(r40 *m);
void    r40_free(r40 *m);
int     r40_load_rom(r40 *m, const char *path);
void    r40_power(r40 *m, int on);
int     r40_step(r40 *m);
int     r40_run(r40 *m, double seconds);
double  r40_time(const r40 *m);
uint8_t r40_peek(r40 *m, uint32_t a);
void    r40_poke(r40 *m, uint32_t a, uint8_t v);
int     r40_event_pop(r40 *m, r40_event *e);
void    r40_breakpoint(r40 *m, uint32_t a, int on);
void    r40_key(r40 *m, int row, int col, int down);
void    r40_set_ptt(r40 *m, int down);
void    r40_set_onoff(r40 *m, int down);
void    r40_set_hook(r40 *m, int offhook);
void    r40_modem_rx(r40 *m, uint8_t b);
double  r40_pll_hz(const r40 *m, int which);

#endif
