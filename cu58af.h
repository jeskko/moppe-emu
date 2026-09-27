/*
 * CU58AF alphanumeric handset, I2C bus (SCL = OUT2 bit 6, SDA = PIO B3,
 * /INT = SIO A /CTS).
 *
 * Devices (8-bit address byte; CU58AF manual p3-5, firmware r58.asm L726-760):
 *   0x40 PCF8574  COLBTN: b0-2 keypad columns, b3 POWER (wire 1 = pressed),
 *                 b4 LDR (wire 0 = bright), b5 SPEAKER, b6 TANGENT/PTT
 *                 (wire 0 = pressed), b7 HOOK (wire 1 = off hook)
 *   0x4C PCF8574  keypad rows
 *   0x44 PCF8574  LEDs/backlight
 *   0x7C PCF8574A audio control
 *   0x48 PCD3312  DTMF generator
 *   0x70 PCF8576  LCD driver(s), 1:4 multiplex, 40 data bytes
 *
 * Keypad matrix follows the firmware's keytbl_cu58af: key at (row, col)
 * shorts ROW bit `row` to COLBTN bit `col`.
 */
#ifndef CU58AF_H
#define CU58AF_H

#include <stdint.h>

typedef struct pcf8574 {
	uint8_t addr;		/* 8-bit write address */
	uint8_t latch;		/* output latch (1 = weak pull-up / input) */
	uint8_t ext_low;	/* external pull-downs, excluding the key matrix */
	uint8_t snapshot;	/* pin state at last read/write, for /INT */
} pcf8574;

typedef struct cu58af {
	/* bus */
	uint8_t scl, sda_master;	/* master drives: sda_master 0 = low */
	uint8_t sda_slave;		/* 0 = some slave pulls low */
	uint8_t last_sda;
	int     state;			/* 0 idle, 1 address, 2 write, 3 read */
	int     bit;			/* bits clocked in the current byte */
	uint8_t shreg;
	int     ack_phase;
	int     dev;			/* addressed device index, -1 none */
	uint8_t rbyte;			/* byte being sent to master */
	int     first_data;		/* LCD: first byte after address */
	/* devices */
	pcf8574 colbtn, row, led, ctrl;
	uint8_t dtmf;			/* last PCD3312 code */
	/* PCF8576 */
	uint8_t lcd_ram[40];
	uint8_t lcd_ptr;
	uint8_t lcd_mode, lcd_dev, lcd_bank, lcd_blink;
	int     lcd_cmd;		/* expecting commands (vs data) */
	unsigned frames;		/* LCD transactions completed */
	/* inputs */
	int     key_row, key_col;	/* pressed matrix key, -1 none */
	uint8_t ptt, speaker, offhook, power, dark;
} cu58af;

void cu58af_init(cu58af *c);
/* master side of the bus */
void cu58af_set_scl(cu58af *c, int level);
void cu58af_set_sda(cu58af *c, int level);	/* 1 = released */
int  cu58af_sda(const cu58af *c);		/* line level */
int  cu58af_int(const cu58af *c);		/* /INT level: 0 = asserted */
/* key by firmware key char ('0'..'9', '*', '#', 'S', 'R', 'C', 'E',
 * '+', '-', 'B'); 0 releases. Returns 0 if unknown. */
int  cu58af_set_key(cu58af *c, int key);
/* 16-bit glyph word of character cell n (0..19) */
uint16_t cu58af_cell(const cu58af *c, int n);

#endif
