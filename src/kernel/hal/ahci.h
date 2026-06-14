#pragma once
#include <stdint.h>

#define AHCI_MAX_PORTS 32

int ahci_init(void);
int ahci_read(int port, uint64_t lba, void *buf, int count);
int ahci_write(int port, uint64_t lba, const void *buf, int count);
int ahci_port_count(void);
