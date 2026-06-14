#pragma once
#include <stdint.h>

#define PCI_MAX_DEVICES 64

typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor;
    uint16_t device;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint32_t bar[6];
    int      is_bridge;
    uint8_t  secondary_bus;
    uint8_t  header_type;
} pci_dev_t;

int  pci_init(void);
int  pci_device_count(void);
pci_dev_t *pci_device(int idx);

uint32_t pci_read_config(pci_dev_t *dev, int reg, int width);
void     pci_write_config(pci_dev_t *dev, int reg, uint32_t val, int width);

pci_dev_t *pci_find(uint8_t class_code, uint8_t subclass, uint8_t prog_if);
void pci_enable_bus_master(pci_dev_t *dev);
void pci_enable_mmio(pci_dev_t *dev);
