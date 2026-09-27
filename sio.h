/*
 * Z80 SIO/0, asynchronous mode only (what the R58 uses: MBUS on B, GPS on A).
 *
 * The board feeds received bytes with sio_rx_byte(), advances time with
 * sio_tick() so transmitted bytes leave at the programmed baud rate, and
 * drives the modem-control inputs with sio_set_status() using the levels
 * as they appear in RR0 (i.e. after the SIO's own /CTS, /DCD inversion).
 */
#ifndef SIO_H
#define SIO_H

#include <stdint.h>
#include "daisy.h"

#define SIO_RR0_RCA    0x01
#define SIO_RR0_INTP   0x02
#define SIO_RR0_TBE    0x04
#define SIO_RR0_DCD    0x08
#define SIO_RR0_SYNC   0x10
#define SIO_RR0_CTS    0x20
#define SIO_RR0_BREAK  0x80
#define SIO_RR0_STATUS (SIO_RR0_DCD | SIO_RR0_SYNC | SIO_RR0_CTS | SIO_RR0_BREAK)

enum { SIO_SRC_RX, SIO_SRC_TX, SIO_SRC_ESC, SIO_SRC_SPECIAL };

typedef struct sio_chan {
	uint8_t wr[8];
	uint8_t ptr;
	uint8_t rxfifo[3];
	uint8_t rxn;
	uint8_t first_char;	/* WR1 "int on first char" armed */
	uint8_t status;		/* live RR0 status bits */
	uint8_t latched;	/* latched RR0 status bits */
	uint8_t latch_valid;	/* status latched since last reset ext/status */
	uint8_t txbuf;
	uint8_t txbuf_full;
	uint8_t txshift_busy;
	double  txdone_at;	/* time (seconds) the shifting byte completes */
	uint8_t tx_int_armed;	/* cleared by "reset TX int pending" */
	double  baud;		/* configured by the board (clock / divisor) */
	irqsrc  irq[3];		/* RX, TX, ESC (special receive shares RX) */
	/* board hooks */
	void  (*tx_byte)(void *ctx, int chan, uint8_t v);
	void  (*wr5_changed)(void *ctx, int chan, uint8_t wr5);
	void   *ctx;
} sio_chan;

typedef struct sio {
	sio_chan ch[2];		/* 0 = A, 1 = B */
	double   clock_hz[2];	/* TxC/RxC per channel */
} sio;

void    sio_init(sio *s);
void    sio_reset(sio *s);
void    sio_write_ctrl(sio *s, int ch, uint8_t v);
uint8_t sio_read_ctrl(sio *s, int ch);
void    sio_write_data(sio *s, int ch, uint8_t v);
uint8_t sio_read_data(sio *s, int ch);
/* returns 0 if the RX FIFO overflowed */
int     sio_rx_byte(sio *s, int ch, uint8_t v);
int     sio_rx_ready(const sio *s, int ch);	/* receiver enabled */
void    sio_set_status(sio *s, int ch, uint8_t mask, uint8_t levels);
void    sio_tick(sio *s, double now);
/* register the six interrupt sources in hardware priority order */
void    sio_add_irqs(sio *s, daisy *d);

#endif
