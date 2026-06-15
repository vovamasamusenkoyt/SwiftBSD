#include "pci.h"
#include "kernel.h"
#include <arch/x86_64/portio.h>

#define CONFIG_ADDR  0xCF8
#define CONFIG_DATA  0xCFC

static pci_dev_t devices[PCI_MAX_DEVICES];
static int ndevices;

static uint32_t pci_config_read(uint8_t bus, uint8_t slot, uint8_t func, int reg) {
    uint32_t addr = (uint32_t)1 << 31
                  | (uint32_t)bus << 16
                  | (uint32_t)slot << 11
                  | (uint32_t)func << 8
                  | (reg & 0xFC);
    outl(CONFIG_ADDR, addr);
    return inl(CONFIG_DATA);
}

static void pci_config_write(uint8_t bus, uint8_t slot, uint8_t func, int reg, uint32_t val) {
    uint32_t addr = (uint32_t)1 << 31
                  | (uint32_t)bus << 16
                  | (uint32_t)slot << 11
                  | (uint32_t)func << 8
                  | (reg & 0xFC);
    outl(CONFIG_ADDR, addr);
    outl(CONFIG_DATA, val);
}

static void add_device(int bus, int slot, int func) {
    if (ndevices >= PCI_MAX_DEVICES) return;

    pci_dev_t *d = &devices[ndevices];
    d->bus  = bus;
    d->slot = slot;
    d->func = func;

    uint32_t id = pci_config_read(bus, slot, func, 0);
    d->vendor = id & 0xFFFF;
    d->device = id >> 16;

    if (d->vendor == 0xFFFF) return;

    uint32_t cc = pci_config_read(bus, slot, func, 8);
    d->class_code = (cc >> 24) & 0xFF;
    d->subclass   = (cc >> 16) & 0xFF;
    d->prog_if    = (cc >> 8)  & 0xFF;

    uint32_t hdr = pci_config_read(bus, slot, func, 0x0C);
    d->header_type = (hdr >> 16) & 0xFF;

    for (int i = 0; i < 6; i++) {
        d->bar[i] = pci_config_read(bus, slot, func, 0x10 + i * 4);
    }

    d->is_bridge = (d->header_type & 0x7F) == 0x01;
    if (d->is_bridge) {
        uint32_t bus_nums = pci_config_read(bus, slot, func, 0x18);
        d->secondary_bus = (bus_nums >> 8) & 0xFF;
    }

    ndevices++;
}

static void scan_bus(int bus) {
    for (int slot = 0; slot < 32; slot++) {
        uint32_t id = pci_config_read(bus, slot, 0, 0);
        if ((id & 0xFFFF) == 0xFFFF) continue;

        add_device(bus, slot, 0);

        uint32_t hdr = pci_config_read(bus, slot, 0, 0x0C);
        int multifunc = (hdr >> 23) & 1;
        if (multifunc) {
            for (int func = 1; func < 8; func++) {
                id = pci_config_read(bus, slot, func, 0);
                if ((id & 0xFFFF) != 0xFFFF)
                    add_device(bus, slot, func);
            }
        }
    }
}

static void scan_bus_recursive(int bus) {
    scan_bus(bus);

    for (int i = 0; i < ndevices; i++) {
        if (devices[i].is_bridge && devices[i].bus == bus) {
            int sec = devices[i].secondary_bus;
            if (sec && sec != bus)
                scan_bus_recursive(sec);
        }
    }
}

int pci_init(void) {
    ndevices = 0;
    scan_bus_recursive(0);

    serial_printf("[pci] %d devices\n", ndevices);
    return ndevices;
}

int pci_device_count(void) { return ndevices; }
pci_dev_t *pci_device(int idx) {
    return (idx >= 0 && idx < ndevices) ? &devices[idx] : 0;
}

uint32_t pci_read_config(pci_dev_t *dev, int reg, int width) {
    uint32_t v = pci_config_read(dev->bus, dev->slot, dev->func, reg);
    if (width == 1) return v & 0xFF;
    if (width == 2) return v & 0xFFFF;
    return v;
}

void pci_write_config(pci_dev_t *dev, int reg, uint32_t val, int width) {
    if (width == 1 || width == 2) {
        uint32_t cur = pci_config_read(dev->bus, dev->slot, dev->func, reg);
        uint32_t mask = (width == 1) ? 0xFF : 0xFFFF;
        int shift = (reg & 3) * 8;
        val = mask ? (cur & ~(mask << shift)) | ((val & mask) << shift) : val;
    }
    pci_config_write(dev->bus, dev->slot, dev->func, reg, val);
}

pci_dev_t *pci_find(uint8_t class_code, uint8_t subclass, uint8_t prog_if) {
    for (int i = 0; i < ndevices; i++) {
        if (devices[i].class_code == class_code &&
            devices[i].subclass == subclass &&
            devices[i].prog_if == prog_if)
            return &devices[i];
    }
    return 0;
}

void pci_enable_bus_master(pci_dev_t *dev) {
    uint32_t cmd = pci_read_config(dev, 4, 4);
    cmd |= 4;
    pci_write_config(dev, 4, cmd, 4);
}

void pci_enable_mmio(pci_dev_t *dev) {
    uint32_t cmd = pci_read_config(dev, 4, 4);
    cmd |= 2;
    pci_write_config(dev, 4, cmd, 4);
}
