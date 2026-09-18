#ifndef DNS_H
#define DNS_H
#include <stdint.h>
#include <stdbool.h>

bool dns_resolve(const char* hostname, uint32_t* out_ip, uint32_t timeout_ms);

#endif
