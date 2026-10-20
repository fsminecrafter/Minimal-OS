#include <stdint.h>
#include <stdbool.h>
#include "x86_64/port.h"
#include "x86_64/ps2.h"
#include "serial.h"

#define PORT_PS2_DATA   0x60
#define PORT_PS2_STATUS 0x64   // read
#define PORT_PS2_CMD    0x64   // write

#define PS2_STATUS_OUT_FULL 0x01
#define PS2_STATUS_IN_FULL  0x02

uint8_t ps2_read_scan_code() {
	return port_inb(PORT_PS2_DATA);
}

static bool ps2_wait_write(void) {
	for (uint32_t i = 0; i < 200000; i++) {
		if (!(port_inb(PORT_PS2_STATUS) & PS2_STATUS_IN_FULL)) return true;
	}
	return false;
}

static bool ps2_wait_read(void) {
	for (uint32_t i = 0; i < 200000; i++) {
		if (port_inb(PORT_PS2_STATUS) & PS2_STATUS_OUT_FULL) return true;
	}
	return false;
}

static void ps2_flush(void) {
	for (int i = 0; i < 64 && (port_inb(PORT_PS2_STATUS) & PS2_STATUS_OUT_FULL); i++) {
		(void)port_inb(PORT_PS2_DATA);
	}
}

static bool ps2_command(uint8_t cmd) {
	if (!ps2_wait_write()) return false;
	port_outb(PORT_PS2_CMD, cmd);
	return true;
}

static bool ps2_keyboard_send(uint8_t byte) {
	for (int attempt = 0; attempt < 3; attempt++) {
		if (!ps2_wait_write()) return false;
		port_outb(PORT_PS2_DATA, byte);
		if (!ps2_wait_read()) return false;
		uint8_t reply = port_inb(PORT_PS2_DATA);
		if (reply == 0xFA) return true;     // ACK
		if (reply != 0xFE) return false;    // anything but "resend"
	}
	return false;
}

/*
 * Puts the 8042 controller and the keyboard into a known state: IRQ1 on,
 * scan code translation on (the driver expects set 1), keyboard scanning.
 *
 * Firmware usually leaves this set up, but not always: UEFI firmware in
 * particular may hand over a controller with IRQs or the keyboard
 * disabled, and then no key ever arrives. Interrupts must be off.
 */
bool ps2_controller_init(void) {
	uint8_t status = port_inb(PORT_PS2_STATUS);
	if (status == 0xFF) {
		serial_write_str("PS/2: no controller\n");
		return false;
	}

	ps2_command(0xAD);              // disable port 1
	ps2_command(0xA7);              // disable port 2
	ps2_flush();

	if (!ps2_command(0x20) || !ps2_wait_read()) {
		serial_write_str("PS/2: controller does not answer\n");
		return false;
	}
	uint8_t config = port_inb(PORT_PS2_DATA);
	config |= 0x01;                 // port 1 interrupt
	config |= 0x40;                 // translate to scan code set 1
	config &= (uint8_t)~0x10;       // port 1 clock on
	if (ps2_command(0x60) && ps2_wait_write()) port_outb(PORT_PS2_DATA, config);

	ps2_command(0xAE);              // enable port 1
	ps2_flush();

	bool ok = ps2_keyboard_send(0xF6) &&    // defaults
	          ps2_keyboard_send(0xF4);      // enable scanning
	ps2_flush();
	serial_write_str(ok ? "PS/2: keyboard enabled\n"
	                    : "PS/2: no keyboard answered (controller set up anyway)\n");
	return ok;
}
