/*
 * Mobira CU41 control unit (MC25 TVL/PTL): PD9 display, PT4 keypad,
 * PX12 interface; no CPU of its own.
 *
 * Display: a 16-cell fluorescent display controller (NEC LD8232 tube,
 * 14 characters in cells 0..13; cells 14 and 15 light the LEDs through
 * their "character" codes).  Bits arrive MSB first on SDO, latched on the
 * DPYCLK rising edge.  A byte below 0x80 is written at the cursor, which
 * then advances and wraps at 16; 0x6E sets the decimal point of the cell
 * before the cursor; bytes from 0x80 up are commands and home the cursor
 * (the firmware sends AF C0 FE once after /RST).  /RST (DPY_ON low)
 * clears it.  Inferred from the OH5NXO firmware's dpyinit (14 x '8', two
 * LED codes, then a 14-cell banner) and its 16-byte frames (LEDs first),
 * which then land LEDs in 14-15 and text in 0-13; not from a data sheet.
 *
 * Keypad (CU41 text, PX12): a key raises DA, which parallel-loads the
 * shift register and drives SDI high while the key is held; CLK1 pulses
 * are gated off.  After release each CLK1 trailing edge shifts out the
 * next bit: a 0 start bit, then the 8-bit key code LSB first.  Idle:
 * CLK1 high, SDI low.
 */
#ifndef CU41_H
#define CU41_H

#include <stdint.h>

#define CU41_CELLS 16
#define CU41_DP    0x6E

typedef struct cu41 {
	/* display */
	uint8_t  cell[CU41_CELLS];	/* 0..13 tube left to right, 14-15 LEDs */
	uint8_t  pos;			/* write cursor */
	uint16_t dp;			/* decimal point per cell */
	uint8_t  rx, nbits;
	uint8_t  dpyclk, rst;
	unsigned bytes, commands;	/* received counters */
	uint8_t  last_command;
	uint8_t  last_byte;
	/* keypad */
	int      key;			/* code held, -1 none */
	uint8_t  keyclk;
	uint16_t out;			/* bits still to shift out, LSB first */
	int      nout;
	uint8_t  sdi;
} cu41;

void cu41_init(cu41 *c);
/* OUT 1 bits: 0 SDO, 1 KEYCLK, 2 DPYCLK, 3 DPY_ON (/RST) */
void cu41_out1(cu41 *c, uint8_t v);
int  cu41_sdi(const cu41 *c);
void cu41_key(cu41 *c, int code);	/* raw key code, -1 = release */

#endif
