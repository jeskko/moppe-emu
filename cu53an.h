/*
 * CU53AN numeric handset.
 *
 * Hardware (CU53AN service manual p6-7, p13):
 *   HEF4555 decodes CS2:CS1: 00 keypad load, 01 latch, 10 LCD IC8 ("LCD1"),
 *   11 LCD IC7 ("LCD2").  Two PCF2111 LCD drivers: a 0 start bit,
 *   32 data bits and a backplane-select bit, loaded when the chip enable
 *   drops; transfers of other lengths are ignored.  Two HEF4035 form an
 *   8-bit shift chain DP -> ... -> DCU, clocked by CLK; with CS=00 a clock
 *   parallel-loads it with {LDR, keycode[4:0], 1, 1}.  A HEF40373 latch is
 *   transparent while CS=01 and drives the indicator LEDs/lamps.
 *   MM74C923 keypad encoder: DA high while a key is held.
 */
#ifndef CU53AN_H
#define CU53AN_H

#include <stdint.h>

/* raw MM74C923 key codes, 4*(row-1)+(col-1) */
enum {
	CU53_KEY_3 = 0, CU53_KEY_2 = 1, CU53_KEY_1 = 2, CU53_KEY_CL = 3,
	CU53_KEY_6 = 4, CU53_KEY_5 = 5, CU53_KEY_4 = 6, CU53_KEY_STO = 7,
	CU53_KEY_9 = 8, CU53_KEY_8 = 9, CU53_KEY_7 = 10, CU53_KEY_RCL = 11,
	CU53_KEY_HASH = 12, CU53_KEY_0 = 13, CU53_KEY_STAR = 14, CU53_KEY_ENT = 15,
	CU53_KEY_SHIFT = 16, CU53_KEY_MINUS = 18, CU53_KEY_PLUS = 19,
	CU53_KEY_NONE = -1
};

typedef struct cu53an {
	/* bus state */
	uint8_t  sel;		/* current CS2:CS1 */
	uint8_t  clk;
	uint8_t  shift;		/* 8-bit chain, bit 7 is the DCU output */
	/* LCD drivers: [chip][half] -> 32 segment bits */
	uint32_t lcd[2][2];
	uint64_t rx;		/* bits collected for the selected LCD chip */
	int      rxn;
	/* indicator latch outputs, in firmware bit order (bit 0 first out) */
	uint8_t  latch;
	/* keypad */
	int      key;		/* raw code held, or CU53_KEY_NONE */
	uint8_t  ldr_dark;	/* LDR input bit as loaded into the chain */
	uint8_t  bit1;		/* parallel-load bit 1 (1 on the CU53AN; the
				 * Talkman CU53/CU59 use it as an extra key bit) */
	unsigned frames;	/* complete LCD loads, for change detection */
} cu53an;

void cu53an_init(cu53an *c);
/* OUT2 write: CS1 = bit 4, CS2 = bit 5, CLK = bit 6, DP = bit 7 */
void cu53an_out2(cu53an *c, uint8_t out2);
/* DCU line level as seen on PIO B3 */
int  cu53an_dcu(const cu53an *c);
/* DA line level (1 = key held) */
int  cu53an_da(const cu53an *c);
void cu53an_set_key(cu53an *c, int code);
/* segment bit at firmware position p (0..127): 64*half + 32*chip + j */
int  cu53an_segment(const cu53an *c, int pos);

#endif
