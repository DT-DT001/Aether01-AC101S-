#pragma once

#include <stdint.h>

typedef int err_t;
typedef struct { uint8_t last_octet; } ip_addr_t;
typedef void (*dns_found_callback)(const char *, const ip_addr_t *, void *);
enum { ERR_OK = 0, ERR_INPROGRESS = -5, ERR_MEM = -1, LWIP_DNS_ADDRTYPE_IPV4 = 0, IP4ADDR_STRLEN_MAX = 16 };
err_t dns_gethostbyname_addrtype(const char *name, ip_addr_t *ip, dns_found_callback callback,
                                 void *argument, uint8_t addr_type);
char *ipaddr_ntoa_r(const ip_addr_t *address, char *text, int size);
