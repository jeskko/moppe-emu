#include "cu58af.h"

#include <string.h>

enum { ST_IDLE, ST_ADDR, ST_WRITE, ST_READ };
enum { DEV_NONE = -1, DEV_COLBTN, DEV_ROW, DEV_LED, DEV_CTRL, DEV_DTMF, DEV_LCD };

/* firmware keytbl_cu58af: index = col + 3*row */
static const char keymap[8][3] = {
	{ 0,   0,   'B' },
	{ '4', '2', '1' },
	{ '5', '6', '3' },
	{ '7', '8', '9' },
	{ '*', '0', '#' },
	{ '-', 'C', 'E' },
	{ '+', 'S', 'R' },
	{ 0,   0,   0   },
};

void
cu58af_init(cu58af *c)
{
	memset(c, 0, sizeof(*c));
	c->scl = 1;
	c->sda_master = 1;
	c->sda_slave = 1;
	c->last_sda = 1;
	c->dev = DEV_NONE;
	c->colbtn.addr = 0x40;
	c->row.addr = 0x4c;
	c->led.addr = 0x44;
	c->ctrl.addr = 0x7c;
	c->colbtn.latch = c->row.latch = c->led.latch = c->ctrl.latch = 0xff;
	c->key_row = c->key_col = -1;
	c->lcd_cmd = 1;
	/* at rest: power button up (b3 low), bright (b4 low), speaker and
	 * PTT released (high), on hook (b7 low) */
	c->dark = 0;
	c->offhook = 0;
}

/* ---------------------------------------------------------------- ports */

static uint8_t
colbtn_ext_low(const cu58af *c)
{
	uint8_t low = 0;
	if (!c->power)   low |= 0x08;
	if (!c->dark)    low |= 0x10;
	if (c->speaker)  low |= 0x20;
	if (c->ptt)      low |= 0x40;
	if (!c->offhook) low |= 0x80;
	return low;
}

/* pin levels of COLBTN and ROW including the key matrix */
static void
matrix_pins(const cu58af *c, uint8_t *colpins, uint8_t *rowpins)
{
	uint8_t col = c->colbtn.latch & ~colbtn_ext_low(c);
	uint8_t row = c->row.latch;

	if (c->key_row >= 0 && c->key_col >= 0) {
		uint8_t rb = 1 << c->key_row, cb = 1 << c->key_col;
		int level = (col & cb) && (row & rb);
		if (!level) {
			col &= ~cb;
			row &= ~rb;
		}
	}
	*colpins = col;
	*rowpins = row;
}

static uint8_t
port_pins(const cu58af *c, int dev)
{
	uint8_t col, row;

	switch (dev) {
	case DEV_COLBTN: matrix_pins(c, &col, &row); return col;
	case DEV_ROW:    matrix_pins(c, &col, &row); return row;
	case DEV_LED:    return c->led.latch;
	case DEV_CTRL:   return c->ctrl.latch;
	}
	return 0xff;
}

static pcf8574 *
port(cu58af *c, int dev)
{
	switch (dev) {
	case DEV_COLBTN: return &c->colbtn;
	case DEV_ROW:    return &c->row;
	case DEV_LED:    return &c->led;
	case DEV_CTRL:   return &c->ctrl;
	}
	return 0;
}

int
cu58af_int(const cu58af *c)
{
	/* open-drain wired-OR of the input ports' interrupt outputs */
	if (port_pins(c, DEV_COLBTN) != c->colbtn.snapshot)
		return 0;
	if (port_pins(c, DEV_ROW) != c->row.snapshot)
		return 0;
	return 1;
}

/* ---------------------------------------------------------------- LCD */

static void
lcd_byte(cu58af *c, uint8_t b)
{
	if (c->lcd_cmd) {
		int more = b & 0x80;
		uint8_t cmd = b & 0x7f;
		if ((cmd & 0x60) == 0x40)		/* 10x E B M1 M0: mode set */
			c->lcd_mode = cmd & 0x1f;
		else if ((cmd & 0x40) == 0)		/* 0 P5..P0: data pointer */
			c->lcd_ptr = cmd & 0x3f;
		else if ((cmd & 0x78) == 0x60)		/* 1100 A2..A0: device select */
			c->lcd_dev = cmd & 7;
		else if ((cmd & 0x7c) == 0x78)		/* 11110 I O: bank select */
			c->lcd_bank = cmd & 3;
		else if ((cmd & 0x78) == 0x70)		/* 1110 A BF1 BF0: blink */
			c->lcd_blink = cmd & 7;
		if (!more)
			c->lcd_cmd = 0;
		return;
	}
	/* 1:4 multiplex: one byte = 2 segment columns; the pointer counts
	 * columns, so bytes land at ptr/2 across the cascaded drivers */
	unsigned idx = c->lcd_dev * 20 + c->lcd_ptr / 2;
	if (idx < sizeof(c->lcd_ram))
		c->lcd_ram[idx] = b;
	c->lcd_ptr += 2;
	if (c->lcd_ptr >= 40) {
		c->lcd_ptr = 0;
		c->lcd_dev++;
	}
}

uint16_t
cu58af_cell(const cu58af *c, int n)
{
	if (n < 0 || n >= 20)
		return 0;
	return c->lcd_ram[2 * n] | (c->lcd_ram[2 * n + 1] << 8);
}

/* ---------------------------------------------------------------- bus */

static int
match_addr(uint8_t a)
{
	switch (a & 0xfe) {
	case 0x40: return DEV_COLBTN;
	case 0x4c: return DEV_ROW;
	case 0x44: return DEV_LED;
	case 0x7c: return DEV_CTRL;
	case 0x48: return DEV_DTMF;
	case 0x70: return DEV_LCD;
	}
	return DEV_NONE;
}

static void
write_byte(cu58af *c, uint8_t b)
{
	pcf8574 *p = port(c, c->dev);

	if (p) {
		p->latch = b;
		p->snapshot = port_pins(c, c->dev);
		/* writing one matrix port changes the other's pins too */
		if (c->dev == DEV_COLBTN)
			c->row.snapshot = port_pins(c, DEV_ROW);
		else if (c->dev == DEV_ROW)
			c->colbtn.snapshot = port_pins(c, DEV_COLBTN);
		return;
	}
	if (c->dev == DEV_DTMF)
		c->dtmf = b;
	else if (c->dev == DEV_LCD)
		lcd_byte(c, b);
}

static uint8_t
read_byte(cu58af *c)
{
	pcf8574 *p = port(c, c->dev);

	if (!p)
		return 0xff;
	uint8_t v = port_pins(c, c->dev);
	p->snapshot = v;
	return v;
}

static void
bus_stop(cu58af *c)
{
	if (c->dev == DEV_LCD)
		c->frames++;
	c->state = ST_IDLE;
	c->dev = DEV_NONE;
	c->sda_slave = 1;
}

static void
bus_edge(cu58af *c, int old_scl, int old_sda)
{
	int sda = cu58af_sda(c);

	/* START / STOP: SDA changes while SCL stays high */
	if (c->scl && old_scl && sda != old_sda) {
		if (!sda) {
			c->state = ST_ADDR;
			c->bit = 0;
			c->shreg = 0;
			c->ack_phase = 0;
			c->dev = DEV_NONE;
			c->sda_slave = 1;
		} else
			bus_stop(c);
		return;
	}
	if (c->state == ST_IDLE)
		return;

	if (c->scl && !old_scl) {
		/* rising SCL: sample */
		if (c->ack_phase)
			return;			/* 9th clock: nothing to sample */
		if (c->state == ST_READ) {
			c->bit++;
			return;
		}
		c->shreg = (uint8_t)((c->shreg << 1) | sda);
		c->bit++;
		return;
	}
	if (!c->scl && old_scl) {
		/* falling SCL: slaves change SDA */
		if (c->ack_phase) {
			/* end of 9th clock */
			c->ack_phase = 0;
			c->sda_slave = 1;
			c->bit = 0;
			if (c->state == ST_READ) {
				c->rbyte = read_byte(c);
				c->sda_slave = (c->rbyte >> 7) & 1;
			}
			return;
		}
		if (c->state == ST_READ) {
			if (c->bit < 8) {
				c->sda_slave = (c->rbyte >> (7 - c->bit)) & 1;
			} else {
				c->sda_slave = 1;	/* release for master ACK */
				c->ack_phase = 1;
			}
			return;
		}
		if (c->bit < 8)
			return;
		/* 8 bits received */
		if (c->state == ST_ADDR) {
			c->dev = match_addr(c->shreg);
			if (c->dev == DEV_NONE) {
				c->state = ST_IDLE;
				return;
			}
			c->state = (c->shreg & 1) ? ST_READ : ST_WRITE;
			if (c->dev == DEV_LCD)
				c->lcd_cmd = 1;
		} else
			write_byte(c, c->shreg);
		c->shreg = 0;
		c->sda_slave = 0;		/* ACK */
		c->ack_phase = 1;
	}
}

void
cu58af_set_scl(cu58af *c, int level)
{
	int old_scl = c->scl, old_sda = cu58af_sda(c);

	level = !!level;
	if (level == c->scl)
		return;
	c->scl = level;
	bus_edge(c, old_scl, old_sda);
}

void
cu58af_set_sda(cu58af *c, int level)
{
	int old_sda = cu58af_sda(c);

	c->sda_master = !!level;
	if (cu58af_sda(c) != old_sda)
		bus_edge(c, c->scl, old_sda);
}

int
cu58af_sda(const cu58af *c)
{
	return c->sda_master && c->sda_slave;
}

int
cu58af_set_key(cu58af *c, int key)
{
	if (!key) {
		c->key_row = c->key_col = -1;
		return 1;
	}
	for (int r = 0; r < 8; r++)
		for (int k = 0; k < 3; k++)
			if (keymap[r][k] == key) {
				c->key_row = r;
				c->key_col = k;
				return 1;
			}
	return 0;
}
