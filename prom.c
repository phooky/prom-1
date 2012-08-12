/**
 * \file PROM dancer
 *
 * Read up to 40 pin DIP PROMs using a Teensy++ 2.0
 */

#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/interrupt.h>
#include <stdint.h>
#include <string.h>
#include <util/delay.h>
#include "usb_serial.h"
#include "bits.h"

void send_str(const char *s);
uint8_t recv_str(char *buf, uint8_t size);
void parse_and_execute_command(const char *buf, uint8_t num);

static uint8_t
hexdigit(
	uint8_t x
)
{
	x &= 0xF;
	if (x < 0xA)
		return x + '0' - 0x0;
	else
		return x + 'A' - 0xA;
}


static uint8_t
printable(
	uint8_t x
)
{
	if ('A' <= x && x <= 'Z')
		return 1;
	if ('a' <= x && x <= 'z')
		return 1;
	if ('0' <= x && x <= '9')
		return 1;
	if (x == ' ')
		return 1;

	return 0;
}
	

/** Mapping of AVR IO ports to the ZIF socket pins */
static const uint8_t ports[] = {
	[ 1]	= 0xB6,
	[ 2]	= 0xB5,
	[ 3]	= 0xB4,
	[ 4]	= 0xB3,
	[ 5]	= 0xB2,
	[ 6]	= 0xB1,
	[ 7]	= 0xB0,
	[ 8]	= 0xE7,
	[ 9]	= 0xE6,
	[10]	= 0xA2,
	[11]	= 0xA1,
	[12]	= 0xF0,
	[13]	= 0xF1,
	[14]	= 0xF2,
	[15]	= 0xF3,
	[16]	= 0xF4,
	[17]	= 0xF5,
	[18]	= 0xF6,
	[19]	= 0xF7,
	[20]	= 0xA3,

	[21]	= 0xA7,
	[22]	= 0xC7,
	[23]	= 0xC6,
	[24]	= 0xC5,
	[25]	= 0xC4,
	[26]	= 0xC3,
	[27]	= 0xC2,
	[28]	= 0xC1,
	[29]	= 0xC0,
	[30]	= 0xE1,
	[31]	= 0xE0,
	[32]	= 0xD7,
	[33]	= 0xD6,
	[34]	= 0xD5,
	[35]	= 0xD4,
	[36]	= 0xD3,
	[37]	= 0xD2,
	[38]	= 0xD1,
	[39]	= 0xD0,
	[40]	= 0xB7,
};


typedef struct
{
	char name[16];
	uint8_t pins;
	uint8_t addr_width;
	uint8_t data_width;
	uint8_t addr_pins[24];
	uint8_t data_pins[24];
	uint8_t hi_pins[8];
	uint8_t lo_pins[8];
} prom_t;


static const prom_t proms[] = {
{
	// Default is to leave everything in tristate mode
	.name		= "NONE",
	.pins		= 28,

	.addr_width	= 0,
	.addr_pins	= {
	},

	.data_width	= 0,
	.data_pins	= {
	},

	.hi_pins	= { },
	.lo_pins	= { },
},
{
	.name		= "M27C512",
	.pins		= 28,

	.addr_width	= 16,
	.addr_pins	= {
		10, 9, 8, 7, 6, 5, 4, 3, 25, 24, 21, 23, 2, 26, 27, 1,
	},

	.data_width	= 8,
	.data_pins	= {
		11, 12, 13, 15, 16, 17, 18, 19,
	},

	.hi_pins	= { 28, },
	.lo_pins	= { 22, 20, 14, },
},
{
	.name		= "M27C256",
	.pins		= 28,
	.addr_width	= 15,
	.addr_pins	= {
		10, 9, 8, 7, 6, 5, 4, 3, 25, 24, 21, 23, 2, 26, 27,
	},

	.data_width	= 8,
	.data_pins	= {
		11, 12, 13, 15, 16, 17, 18, 19,
	},
	.hi_pins	= { 28, 1 },
	.lo_pins	= { 22, 20, 14, },
},
{
	/** 8192x8 UV EEPROM, found in DX synth */
	.name		= "MBM2764-30",
	.pins		= 28,
	.addr_width	= 13,
	.addr_pins	= {
		10, 9, 8, 7, 6, 5, 4, 3, 25, 24, 21, 23, 2,
	},

	.data_width	= 8,
	.data_pins	= {
		11, 12, 13, 15, 16, 17, 18, 19,
	},
	.hi_pins	= {
		//28, // vdd, disabled since external power must be used
		27, // pgm
		1,  // vpp
	},
	.lo_pins	= { 22, 20, 14, }, // !oe, !cs, gnd
},
{
	/** Apple Mac SE PROM chips
	 * Similar to a M27C512, but with the 17th address line
	 * on 22 instead of Vpp, allowing 128 KB of data.
	 */
	.name		= "APPLE PROM",
	.pins		= 28,
	.addr_width	= 17,
	.addr_pins	= {
		10, 9, 8, 7, 6, 5, 4, 3, 25, 24, 21, 23, 2, 26, 27, 1, 22
	},

	.data_width	= 8,
	.data_pins	= {
		11, 12, 13, 15, 16, 17, 18, 19,
	},
	.hi_pins	= { 28, },
	.lo_pins	= { 20, 14, },
},
};

/** Select one of the chips */
static const prom_t * prom = &proms[0];


/** Translate PROM pin numbers into ZIF pin numbers */
static inline uint8_t
prom_pin(
	const uint8_t pin
)
{
	if (pin <= prom->pins / 2)
		return ports[pin];
	else
		return ports[pin + 40 - prom->pins];
}
		

/** Select a 32-bit address for the current PROM */
static void
prom_set_address(
	uint32_t addr
)
{
	for (uint8_t i = 0 ; i < prom->addr_width ; i++)
	{
		out(prom_pin(prom->addr_pins[i]), addr & 1);
		addr >>= 1;
	}
}


static uint8_t
_prom_read(void)
{
	uint8_t b = 0;
	for (uint8_t i = 0 ; i < prom->data_width  ; i++)
	{
		uint8_t bit = in(prom_pin(prom->data_pins[i])) ? 0x80 : 0;
		b = (b >> 1) | bit;
	}

	return b;
}


/** Read a byte from the PROM at the specified address..
 * \todo Update this to handle wider than 8-bit PROM chips.
 */
static uint8_t
prom_read(
	uint32_t addr
)
{
	prom_set_address(addr);
	for(uint8_t i = 0 ; i < 255; i++)
	{
		asm("nop");
		asm("nop");
		asm("nop");
		asm("nop");
	}

	uint8_t old_r = _prom_read();

	// Try reading a few times to be sure,
	// or until the values converge
	for (uint8_t i = 0 ; i < 8 ; i++)
	{
		uint8_t r = _prom_read();
		if (r == old_r)
			break;
		old_r = r;
	}

	return old_r;
}


/** Configure all of the IO pins for the new PROM type */
static void
prom_setup(void)
{
	// Configure all of the address pins as outputs
	for (uint8_t i = 0 ; i < prom->addr_width ; i++)
		ddr(prom_pin(prom->addr_pins[i]), 1);

	// Configure all of the data pins as inputs
	for (uint8_t i = 0 ; i < prom->data_width ; i++)
		ddr(prom_pin(prom->data_pins[i]), 0);

	// Configure all of the hi and low pins as outputs
	for (uint8_t i = 0 ; i < array_count(prom->hi_pins) ; i++)
	{
		uint8_t pin = prom_pin(prom->hi_pins[i]);
		if (pin == 0)
			continue;
		out(pin, 1);
		ddr(pin, 1);
	}

	for (uint8_t i = 0 ; i < array_count(prom->lo_pins) ; i++)
	{
		uint8_t pin = prom_pin(prom->lo_pins[i]);
		if (pin == 0)
			continue;
		out(pin, 0);
		ddr(pin, 1);
	}

	// Let things stabilize for a little while
	_delay_ms(250);
}


/** Switch all of the ZIF pins back to tri-state to make it safe.
 * Doesn't matter what PROM is inserted.
 */
static void
prom_tristate(void)
{
	for (uint8_t i = 1 ; i <= 40 ; i++)
	{
		ddr(ports[i], 0);
		out(ports[i], 0);
	}
}



static uint8_t
usb_serial_getchar_block(void)
{
	while (1)
	{
		while (usb_serial_available() == 0)
			continue;

		uint16_t c = usb_serial_getchar();
		if (c != -1)
			return c;
	}
}


static uint8_t
usb_serial_getchar_echo(void)
{
	while (1)
	{
		while (usb_serial_available() == 0)
			continue;

		uint16_t c = usb_serial_getchar();
		if (c == -1)
			continue;
		usb_serial_putchar(c);
		return c;
	}
}

static uint8_t
hexdigit_parse(
	uint8_t c
)
{
	if ('0' <= c && c <= '9')
		return c - '0';
	if ('A' <= c && c <= 'F')
		return c - 'A' + 0xA;
	if ('a' <= c && c <= 'f')
		return c - 'a' + 0xA;
	return 0xFF;
}

static void
hex32(
	uint8_t * buf,
	uint32_t addr
)
{
	buf[7] = hexdigit(addr & 0xF); addr >>= 4;
	buf[6] = hexdigit(addr & 0xF); addr >>= 4;
	buf[5] = hexdigit(addr & 0xF); addr >>= 4;
	buf[4] = hexdigit(addr & 0xF); addr >>= 4;
	buf[3] = hexdigit(addr & 0xF); addr >>= 4;
	buf[2] = hexdigit(addr & 0xF); addr >>= 4;
	buf[1] = hexdigit(addr & 0xF); addr >>= 4;
	buf[0] = hexdigit(addr & 0xF); addr >>= 4;
}


static void
hexdump(
	uint32_t addr
)
{
	uint8_t buf[80];
	hex32(buf, addr);

	for (int i = 0 ; i < 16 ; i++)
	{
		uint8_t w = prom_read(addr++);
		uint8_t x = 8 + i * 3;
		buf[x+0] = ' ';
		buf[x+1] = hexdigit(w >> 4);
		buf[x+2] = hexdigit(w >> 0);

		buf[8 + 16*3 + i + 2] = printable(w) ? w : '.';
	}

	buf[8 + 16 * 3] = ' ';
	buf[8 + 16 * 3 + 18] = '\r';
	buf[8 + 16 * 3 + 19] = '\n';

	usb_serial_write(buf, 8 + 16 * 3 + 20);
}


/** Read an address from the serial port, then read that from the PROM */
static void
read_addr(void)
{
	uint32_t addr = 0;
	while (1)
	{
		uint8_t c = usb_serial_getchar_echo();
		if (c == '\r')
			break;
		if (c == '\n')
			continue;
		uint8_t n = hexdigit_parse(c);
		if (n == 0xFF)
			goto error;

		addr = (addr << 4) | n;
	}

	send_str(PSTR("\r\n"));

	prom_setup();
	_delay_ms(100);

	for (uint8_t line = 0 ; line < 4 ; line++)
	{
		hexdump(addr);
		addr += 16;
	}
	return;

error:
	send_str(PSTR("?\r\n"));
}


/** Send a single prom name to the serial port */
static void
prom_list_send(
	int mode,
	const prom_t * const prom,
	int selected
)
{
	uint8_t buf[32];

	uint8_t off = 0;
	if (selected)
	{
		buf[off++] = '*';
		buf[off++] = '*';
		buf[off++] = '*';
		buf[off++] = ' ';
	}

	buf[off++] = hexdigit(mode);
	buf[off++] = ' ';
	memcpy(buf+off, prom->name, sizeof(prom->name));
	off += sizeof(prom->name);
	buf[off++] = '\r';
	buf[off++] = '\n';

	usb_serial_write(buf, off);
}


/** List all of the PROM models supported */
static void
prom_list(void)
{
	send_str(PSTR("\r\n"));
	for (int i = 0 ; i < array_count(proms) ; i++)
	{
		const prom_t * const p = &proms[i];
		prom_list_send(i, p, p == prom );
	}
}


static void
prom_mode(void)
{
	uint8_t c = usb_serial_getchar_echo();
	send_str(PSTR("\r\n"));
	if (c < '0' || '9' < c)
	{
		send_str(PSTR("?\r\n"));
		return;
	}

	uint8_t mode = c - '0';
	if (mode >= array_count(proms))
	{
		send_str(PSTR("?\r\n"));
		return;
	}

	prom = &proms[mode];

	prom_list_send(mode, prom, 1);
}


typedef struct
{
	uint8_t soh;
	uint8_t block_num;
	uint8_t block_num_complement;
	uint8_t data[128];
	uint8_t cksum;
} __attribute__((__packed__))
xmodem_block_t;

#define XMODEM_SOH 0x01
#define XMODEM_EOT 0x04
#define XMODEM_ACK 0x06
#define XMODEM_CAN 0x18
#define XMODEM_C 0x43
#define XMODEM_NAK 0x15
#define XMODEM_EOF 0x1a


/** Send a block.
 * Compute the checksum and complement.
 *
 * \return 0 if all is ok, -1 if a cancel is requested or more
 * than 10 retries occur.
 */
static int
xmodem_send_block(
	xmodem_block_t * const block
)
{
	// Compute the checksum and complement
	uint8_t cksum = 0;
	for (uint8_t i = 0 ; i < sizeof(block->data) ; i++)
		cksum += block->data[i];
	block->cksum = cksum;
	block->block_num_complement = 0xFF - block->block_num;

	// Send the block, and wait for an ACK
	uint8_t retry_count = 0;
	goto send_block;

	while (retry_count++ < 10)
	{
		uint8_t c = usb_serial_getchar_block();
		if (c == XMODEM_ACK)
			return 0;
		if (c == XMODEM_CAN)
			return -1;
		if (c != XMODEM_NAK)
			continue;

		send_block:
		usb_serial_write((void*) block, sizeof(*block));
	}

	// Failure or cancel
	return -1;
}


/** Send the entire PROM memory */
static void
xmodem_send(void)
{
	uint8_t c;
	static xmodem_block_t block;

	block.soh = 0x01;
	block.block_num = 0x00;

	// wait for initial nak
	while (1)
	{
		c = usb_serial_getchar_block();
		if (c == XMODEM_NAK)
			break;
		if (c == XMODEM_CAN)
			return;
	}

	// Ending address
	const uint32_t end_addr = (((uint32_t) 1) << prom->addr_width) - 1;

	// Bring the pins up to level
	prom_setup();

	// Start sending!
	uint32_t addr = 0;
	while (1)
	{
		block.block_num++;
		for (uint8_t off = 0 ; off < sizeof(block.data) ; off++)
			block.data[off] = prom_read(addr++);

		if (xmodem_send_block(&block) < 0)
			return;

		// If we have wrapped the address, we are done
		if (addr >= end_addr)
			break;
	}

#if 0
/* Don't send EOF?  rx adds it to the file? */
	block.block_num++;
	memset(block.data, XMODEM_EOF, sizeof(block.data));
	if (xmodem_send_block(&block) < 0)
		return;
#endif

	// File transmission complete.  send an EOT
	while (1)
	{
		usb_serial_putchar(XMODEM_EOT);
		c = usb_serial_getchar_block();
		if (c == XMODEM_ACK
		||  c == XMODEM_CAN)
			break;
	}
}



int main(void)
{
	// set for 16 MHz clock
#define CPU_PRESCALE(n) (CLKPR = 0x80, CLKPR = (n))
	CPU_PRESCALE(0);

	// Disable the ADC
	ADMUX = 0;

	// initialize the USB, and then wait for the host
	// to set configuration.  If the Teensy is powered
	// without a PC connected to the USB port, this 
	// will wait forever.
	usb_init();
	while (!usb_configured()) /* wait */ ;
	_delay_ms(1000);

	// wait for the user to run their terminal emulator program
	// which sets DTR to indicate it is ready to receive.
	while (!(usb_serial_get_control() & USB_SERIAL_DTR))
		continue;


	// discard anything that was received prior.  Sometimes the
	// operating system or other software will send a modem
	// "AT command", which can still be buffered.
	usb_serial_flush_input();


#if 0
	uint16_t addr = 0;
	char line[64];
	uint8_t off = 0;

	send_str(PSTR("Looking for strings\r\n"));

	while (1)
	{
		addr++;
		if (addr == 0)
			send_str(PSTR("wrap\r\n"));

		uint8_t byte = read_byte(addr);
		if (byte == 0)
			continue;

		if (off == 0)
		{
			line[off++] = hexdigit(addr >> 12);
			line[off++] = hexdigit(addr >>  8);
			line[off++] = hexdigit(addr >>  4);
			line[off++] = hexdigit(addr >>  0);
			line[off++] = '=';
		}

		if (printable(byte))
		{
			line[off++] = byte;
			if (off < sizeof(line) - 2)
				continue;
		} else {
			line[off++] = hexdigit(byte >> 4);
			line[off++] = hexdigit(byte >> 0);
		}

		line[off++] = '\r';
		line[off++] = '\n';
		usb_serial_write(line, off);
		off = 0;
	}
#else
	while (1)
	{
		// always put the PROM into tristate so that it is safe
		// to swap the chips in between readings, and 
		prom_tristate();

		send_str(PSTR("> "));
		char c = usb_serial_getchar_echo();
		switch (c)
		{
		case XMODEM_NAK: xmodem_send(); break;
		case 'r': read_addr(); break;
		case 'l': prom_list(); break;
		case 'm': prom_mode(); break;
		case '\n': break;
		case '\r': break;
		default:
			send_str(PSTR("\r\n"
"r000000 Read a hex word from address\r\n"
"l       List chip modes\r\n"
"mN      Select chip N\r\n"
			));
			break;
		}
	}
#endif
}


// Send a string to the USB serial port.  The string must be in
// flash memory, using PSTR
//
void send_str(const char *s)
{
	char c;
	while (1) {
		c = pgm_read_byte(s++);
		if (!c) break;
		usb_serial_putchar(c);
	}
}
