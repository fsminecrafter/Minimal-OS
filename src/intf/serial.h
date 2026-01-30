#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include <stddef.h>

#define SERIAL_COM1 0x3F8  // COM1 base port

void serial_init();
int serial_received();
char serial_read();
int serial_is_transmit_empty();
void serial_write(char a);
void serial_write_str(const char* str);
void serial_write_bin(uint64_t val);
void serial_write_hex(uint64_t val);
void serial_write_dec(uint64_t val);

#endif
