/*
 * Motorola MC68HC16Z1: the CPU16 core (cpu16.c) with the chip's modules:
 * SIM (clock synthesizer, chip selects, ports C/E/F, periodic interrupt
 * timer, software watchdog, interrupt arbitration), 1 KB standby SRAM,
 * GPT (timer, output compares, input captures, PWM), QSM (SCI, port QS)
 * and the 10-bit ADC.  The board sits behind the external bus and pin
 * callbacks.
 *
 * Time is kept in system clocks (chip.clk); the system clock follows
 * SYNCR (8.388 MHz out of reset, 16.777 MHz with the MDR firmware's
 * 7F00).  The PIT and the watchdog count EXTAL (32.768 kHz) cycles.
 *
 * Module registers sit at $FFxxx (SIMCR.MM = 1, which is the reset
 * state; MM is write-once).  Sources: SIM, GPT, QSM and ADC Reference
 * Manuals and the MC68HC16Z1 User's Manual; notes/mdr150.md.
 */
#ifndef HC16Z1_H
#define HC16Z1_H

#include <stdint.h>

#include "cpu16.h"

#define HC16_EXTAL	32768

/* chip-select line bits in the cs masks handed to the board */
#define CS_BOOT		(1u << 11)	/* CSBOOT; CS0..CS10 are bits 0..10 */

/* ports for the pin callbacks */
enum { HC16_PORTC, HC16_PORTE, HC16_PORTF, HC16_PORTGP, HC16_PORTQS, HC16_PORTADA };

typedef struct hc16z1_board {
	void   *ctx;
	/* external bus: one access of size 1 or 2 (a 16-bit port; the chip
	 * splits accesses to 8-bit ports into byte cycles) with the chip
	 * selects asserted for it */
	uint16_t (*ext_read)(void *ctx, uint32_t a, int size, unsigned cs);
	void     (*ext_write)(void *ctx, uint32_t a, int size, uint16_t v, unsigned cs);
	/* pin levels driven by the chip changed (all pins of the port:
	 * outputs as driven, inputs as last read); value, direction mask */
	void     (*pins_out)(void *ctx, int port, uint8_t v, uint8_t ddr);
	/* external levels of a port's pins */
	uint8_t  (*pins_in)(void *ctx, int port);
	/* ADC input channel ch at the sample instant: 0..1023 (VRL..VRH) */
	int      (*analog)(void *ctx, int ch);
	/* SCI: one frame left the TXD pin */
	void     (*sci_tx)(void *ctx, uint16_t frame);
	/* PWMA/PWMB duty changed (takes effect now): value 0..256 of 256 */
	void     (*pwm)(void *ctx, int ch, int duty);
	/* the chip reset itself (watchdog) */
	void     (*reset)(void *ctx, int why);
} hc16z1_board;

/* why for reset */
enum { HC16_RESET_POWER, HC16_RESET_WATCHDOG, HC16_RESET_HALT };

typedef struct hc16z1 {
	cpu16    cpu;
	hc16z1_board board;
	uint64_t clk;			/* system clocks since power-on */

	/* SIM */
	uint16_t simcr, syncr, sypcr, picr, pitr;
	uint8_t  sypcr_written, mm_written;
	uint8_t  rsr;
	uint8_t  portc, porte, ddre, pepar, portf, ddrf, pfpar;
	uint16_t cspar[2];
	uint16_t csbar[12], csor[12];	/* index 0..10 = CS0..CS10, 11 = CSBOOT */
	uint8_t  swsr_last;
	uint64_t ext_frac;		/* EXTAL phase accumulator: clk * 32768 */
	uint32_t pit_count;		/* EXTAL cycles to the next PIT tick */
	uint8_t  pit_pending;
	uint64_t wd_count;		/* EXTAL cycles since the watchdog was serviced */

	/* standby SRAM */
	uint8_t  ram[1024];
	uint16_t rammcr, rambah, rambal;

	/* GPT */
	uint16_t gptmcr, gpticr;
	uint8_t  ddrgp, portgp;
	uint8_t  oc1m, oc1d;
	uint16_t tcnt;
	uint8_t  pactl, pacnt;
	uint16_t tic[3], toc[4], ti4o5;
	uint8_t  tctl1, tctl2, tmsk1, tmsk2, tflg1, tflg2, cforc, pwmc;
	uint8_t  pwma, pwmb, pwmbufa, pwmbufb;
	uint16_t pwmcnt, prescl;
	uint8_t  tmsk2_cpr_written;
	uint32_t tcnt_acc;		/* clocks into the current TCNT period */
	uint8_t  gp_in_last;		/* input capture edge detection */
	uint8_t  tflg1_read, tflg2_read;	/* flags seen set by a read (clear protocol) */
	uint32_t pwm_acc;
	int      pwm_duty[2];

	/* QSM */
	uint16_t qsmcr;
	uint8_t  qilr, qivr;
	uint8_t  portqs, pqspar, ddrqs;
	uint16_t sccr0, sccr1, scsr;
	uint16_t rdr;			/* received data */
	uint16_t tdr;			/* transmit data register */
	uint16_t tsr;			/* frame in the transmit shifter */
	uint8_t  tx_busy;
	uint64_t tx_done;		/* clk at which the frame in the shifter ends */
	uint8_t  scsr_read;		/* SCSR flags seen set by a read */
	uint16_t rxq[512];		/* host to SCI receive queue */
	int      rxq_head, rxq_tail;
	uint64_t rx_next;		/* clk at which the next queued frame completes */
	uint8_t  rx_idle_armed;
	uint16_t spcr[4];
	uint8_t  spsr;

	/* ADC */
	uint16_t adcmcr, adctl0, adctl1, adcstat;
	uint16_t rslt[8];		/* 10-bit results, right justified */
	uint64_t adc_next;		/* clk at which the next conversion completes */
	int      adc_seq;		/* conversion index within the sequence */
	uint8_t  adc_running;

	/* debugging */
	uint32_t unmapped_addr;		/* last external access no chip select took */
	unsigned unmapped_count;
} hc16z1;

void   hc16z1_init(hc16z1 *m, const hc16z1_board *b);
void   hc16z1_reset(hc16z1 *m);
/* one CPU instruction (or interrupt entry) and the modules over its
 * clocks; returns clocks */
int    hc16z1_step(hc16z1 *m);
double hc16z1_fsys(const hc16z1 *m);
/* host side of the SCI: queue a frame for the RXD pin */
void   hc16z1_sci_rx(hc16z1 *m, uint16_t frame);
int    hc16z1_sci_rx_pending(const hc16z1 *m);
double hc16z1_sci_baud(const hc16z1 *m);
/* debugger access through the chip's address decode, no side effects
 * on module registers beyond a plain read */
uint8_t hc16z1_peek(hc16z1 *m, uint32_t a);
void    hc16z1_poke(hc16z1 *m, uint32_t a, uint8_t v);

#endif
