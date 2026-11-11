/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef DRIVERS_USB_USERIAL_PROTO_H
#define DRIVERS_USB_USERIAL_PROTO_H
#if __has_include(<uapi/types.h>)
#include <uapi/types.h>
#else
#include <stdint.h>
#endif

/*
 * USB serial adapters, the part that is only arithmetic: which chip a device is,
 * which endpoints carry the bytes, and the control requests that set its line
 * (speed, data bits, parity, stop bits, flow control) and its DTR and RTS wires.
 * No hardware here: drivers/usb/usb_serial.c sends what these functions build,
 * and tests/host_userial.c checks them on the host.
 *
 * Sources, all public: USB CDC 1.2 and its PSTN subclass (ACM); Silicon Labs
 * AN571 (CP210x); FTDI AN232B-05 (baud divisors); FreeBSD uchcom(4) and the
 * MIT-licensed usb-serial-for-android notes (CH340/CH341 registers).
 */

#define US_NONE   0
#define US_ACM    1   /* USB CDC-ACM: Arduino Uno R3/Leonardo, RP2040, STM32, roboRIO tools */
#define US_CH341  2   /* WCH CH340/CH341: cheap Arduino clones */
#define US_CP210X 3   /* Silicon Labs CP210x: ESP32 boards, many adapters */
#define US_FTDI   4   /* FTDI FT232R/FT-X, and QEMU's usb-serial */

/* What a device offers: the interfaces and the two bulk endpoints of its data. */
typedef struct {
	uint8_t  kind;
	uint8_t  ctrl_iface;       /* where requests go (ACM: the communication interface) */
	uint8_t  data_iface;
	uint8_t  ep_in, ep_out;    /* endpoint addresses, bit 7 set on IN */
	uint16_t mps_in, mps_out;
} us_desc_t;

/* One control request; data[] is sent for OUT requests with len > 0. */
typedef struct {
	uint8_t  rtype, req;
	uint16_t val, idx, len;
	uint8_t  data[16];
} us_req_t;

/* A line setting, decoded from termios. parity: 0 none, 1 odd, 2 even, 3 mark, 4 space. */
typedef struct {
	uint32_t baud;             /* 0: hang up (B0), drop DTR and RTS */
	uint8_t  bits, parity, stop2, rtscts;
} us_line_t;

/* termios c_cflag bits (Linux numbering, which programs and libc use) */
#define US_CBAUD    0x0000100Fu
#define US_BOTHER   0x00001000u
#define US_CSIZE    0x00000030u
#define US_CSTOPB   0x00000040u
#define US_CREAD    0x00000080u
#define US_PARENB   0x00000100u
#define US_PARODD   0x00000200u
#define US_HUPCL    0x00000400u
#define US_CLOCAL   0x00000800u
#define US_CIBAUD   0x100F0000u
#define US_CMSPAR   0x40000000u
#define US_CRTSCTS  0x80000000u

/* The device's kind and endpoints from its descriptors; US_NONE if it is not one. */
int us_match(uint16_t vid, uint16_t pid, const uint8_t *cfg, uint32_t total, us_desc_t *out);

/* c_cflag's speed: a Bxxx code, or with BOTHER the number in ospeed (termios2). */
uint32_t us_cflag_baud(uint32_t cflag, uint32_t ospeed);
/* The Bxxx code for a speed, or BOTHER when there is none. */
uint32_t us_baud_code(uint32_t baud);
/* c_cflag with what the chip cannot do taken out (it reads back the truth). */
uint32_t us_cflag_fix(int kind, uint32_t cflag);
void us_line_from_cflag(uint32_t cflag, uint32_t ospeed, us_line_t *l);

/* Requests for a moment of the port's life. Each returns how many it wrote to r. */
#define US_MAXREQ 6
int us_open_reqs(const us_desc_t *d, us_req_t *r);
int us_close_reqs(const us_desc_t *d, us_req_t *r);
int us_line_reqs(const us_desc_t *d, const us_line_t *l, us_req_t *r);
int us_modem_reqs(const us_desc_t *d, int dtr, int rts, us_req_t *r);
/* Break on or off. FTDI keeps the line setting in the same request, so l is
 * needed. CH341 returns US_BREAK_RMW: its break bit shares a register pair
 * (0x18, 0x05) with the transmitter, so the caller reads the pair (READ_REG
 * 0x95, value 0x1805), computes the new one with us_ch341_break and writes it
 * (WRITE_REG 0x9A, value 0x1805). */
#define US_BREAK_RMW (-2)
int us_break_reqs(const us_desc_t *d, const us_line_t *l, int on, us_req_t *r);
uint16_t us_ch341_break(uint8_t reg18, uint8_t reg05, int on);

/* Bytes received into buf: the payload is moved to the front and its length
 * returned. FTDI starts each packet with two status bytes; *msr gets the modem
 * status bits (TIOCM_CTS/DSR/RNG/CAR) when the chip reports them, else untouched. */
uint32_t us_rx_payload(int kind, uint16_t mps, uint8_t *buf, uint32_t len, uint32_t *msr);

/* Chip divisors, exposed for the tests. 0 on success. */
int us_ch341_divisor(uint32_t baud, uint8_t *pre, uint8_t *div);
int us_ftdi_divisor(uint32_t baud, uint16_t *val, uint16_t *idx);

/* TIOCM_* bits (Linux numbering) */
#define US_TIOCM_DTR 0x002
#define US_TIOCM_RTS 0x004
#define US_TIOCM_CTS 0x020
#define US_TIOCM_CAR 0x040
#define US_TIOCM_RNG 0x080
#define US_TIOCM_DSR 0x100

#endif
