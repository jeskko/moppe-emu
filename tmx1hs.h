/*
 * Nokia HSN-2 / HSF-2 handsets of the TMN-1 / TMF-1 (Talkman 620 / 520):
 * a uPD7810 of their own, two uPD7228 dot-matrix LCD controllers, the
 * keypad, LEDs and DTMF (HSN-2: PCD3312 on bit-banged I2C; HSF-2: the
 * HSIC audio chip).  The MBUS link is the board's (tmx1.c).
 *
 * Hardware facts are read from OH5NXO's hsn2.asm and OK2UCX's hsf2.asm
 * (notes/tmx1.md); no uPD7228 data sheet is in hand, its command set is
 * what both firmwares' use implies.
 */
#ifndef TMX1HS_H
#define TMX1HS_H

#include <stdint.h>

#include "upd7810.h"

enum { TMX1_HS_NONE, TMX1_HS_HSN2, TMX1_HS_HSF2 };

/* handset events passed to on_event */
enum { TMX1HS_EV_DTMF = 1, TMX1HS_EV_ILLEGAL };

typedef struct upd7228 {
	uint8_t ram[128];
	uint8_t chr[128];	/* character code written at this column (char mode), 0 none */
	uint8_t ptr;
	uint8_t mode;		/* 0 write, 1 OR, 2 AND, 3 character */
	int8_t  dir;		/* pointer step: -1, 0, +1 */
	uint8_t on;
	unsigned writes;
} upd7228;

typedef struct tmx1hs {
	int      type;
	upd7810  cpu;
	/* board hooks: MBUS TxD level, events (type, arg) */
	void    *board;
	void   (*on_txd)(void *board, int level);
	void   (*on_event)(void *board, int type, int arg);
	uint8_t  rom[65536];
	uint8_t  ram[8192];

	uint8_t  latch[2];	/* HSN-2 74259s: D0 (rows, backlight), D1 (LCD, LEDs) */
	uint8_t  hsic[4];	/* HSF-2 HSIC registers (0x0400..0x0700) */
	uint8_t  pa, pb, pc;	/* port outputs as seen at the pins (inputs high) */

	/* keypad: pressed[scancode 1..28]; column lines AN4..AN7 */
	uint8_t  pressed[32];
	uint8_t  an_level;
	int      powersw;	/* power key held */
	int      offhook;

	/* LCD serial interface */
	upd7228  lcd[2];	/* chip address 0 and 3 */
	uint8_t  sck, si, cs, cd;
	uint8_t  sr, nbits, first, sel;

	/* HSN-2 PCD3312 on I2C (PC6 SCL, PC7 SDA) */
	uint8_t  scl, sda, i2c_on, i2c_n, i2c_sr, i2c_byte;
	int      dtmf;		/* last tone code, 0 = none */
	unsigned dtmf_writes;
} tmx1hs;

void tmx1hs_init(tmx1hs *h, int type);
void tmx1hs_reset(tmx1hs *h);
void tmx1hs_key(tmx1hs *h, int scancode, int down);
void tmx1hs_set_hook(tmx1hs *h, int offhook);
void tmx1hs_power_key(tmx1hs *h, int down);
/* 8-char middle and bottom rows, 2-char top: text decoded from the
 * character codes the firmware wrote; '?' for a cell with other data */
void tmx1hs_text(tmx1hs *h, char *mid, char *bot, char *top);
/* icon bits: 1 FCN, 2 LEFT, 4 RIGHT, 8 T, 16 ALPHA, 32 KEY, 64 MINUS */
int  tmx1hs_icons(tmx1hs *h);
/* LED bits: 1 ON, 2 SERV, 4 ROAM, 8 CALL, 16 HF, 32 MFT, 64 BACKLIGHT */
int  tmx1hs_leds(tmx1hs *h);

#endif
