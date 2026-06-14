# AHCI Disk Driver + PCI Bus Driver

## Overview

Replace raw embedded user binary model with a proper SATA disk driver,
enabling the kernel to read sectors from a real attached disk via the
AHCI controller.  This is the foundation for a future filesystem layer.

## Scope

- PCI bus driver (Configuration Mechanism #1, full bridge scanning)
- AHCI HBA driver (polled I/O, no interrupts)
- Block device abstraction for future FS integration

Non-goals: filesystem, partition table parsing, write support, IRQ.

## PCI Bus Driver — `hal/pci.c` / `hal/pci.h`

### Data Structures

```c
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
    uint8_t  secondary_bus;   // valid only if is_bridge
} pci_dev_t;
```

Static array of 64 entries, populated by `pci_init()`.

### API

- `void pci_init(void)` — scan bus 0, recurse into PCI-to-PCI bridges
- `uint32_t pci_read_config(pci_dev_t *, int reg, int width)` — 8/16/32 read
- `void pci_write_config(pci_dev_t *, int reg, uint32_t val, int width)` — 8/16/32 write
- `pci_dev_t *pci_find(uint8_t cls, uint8_t sub, uint8_t prog_if)` — linear search
- `void pci_enable_bus_mastering(pci_dev_t *)` — set PCI command bit 2
- `void pci_enable_mmio(pci_dev_t *)` — set PCI command bit 1

### Implementation

- Configuration Mechanism #1 via ports 0xCF8 / 0xCFC
- Bridge detection: header type 0x01 → read secondary bus number from config offset 0x19
- Maximum depth of 8 buses, 32 devices per bus, 8 functions per device

## AHCI Driver — `hal/ahci.c` / `hal/ahci.h`

### Constants / Structures (from AHCI 1.3.1 spec)

ABAR (BAR5) layout:

```
+0x0000  HBA capabilities (CAP)
+0x0004  GHC (global host control)
+0x000C  PI (ports implemented)
+0x0100  Port 0 registers (0x80 bytes each, stride 0x80)
+0x0180  Port 1 registers
...
```

Port registers (offset within port region):

```
+0x00  PxCLB  / PxCLBU  — cmd list base (low/high)
+0x08  PxFB   / PxFBU   — FIS base (low/high)
+0x10  PxIS   — interrupt status
+0x18  PxCMD  — port command
+0x20  PxTFD  — task file data
+0x28  PxSIG  — signature
+0x30  PxSSTS — serial ATA status
+0x38  PxSCTL — serial ATA control
+0x40  PxSERR — serial ATA error
+0x48  PxCI   — command issue
+0x58  PxSACT — serial ATA active
```

Command Header (32 bytes per slot, up to 32 slots):

```
+0x00  DW0: CFL(4) A(1) W(1) PRDTL(16)
+0x04  DW1: PRDBC (byte count transferred, read-only)
+0x08  DW2: CTBA (command table base address, low)
+0x0C  DW3: CTBAU (high)
+0x10  DW4-DW7: reserved
```

Command Table (128 bytes):

```
+0x00  CFIS (64 bytes) — Command FIS
+0x40  ATAPI command (16 bytes, unused)
+0x50  reserved
+0x80  PRDT entries (16 bytes each)
```

PRDT entry (16 bytes):

```
+0x00  DBA (data base address, low)
+0x04  DBAU (high)
+0x08  reserved
+0x0C  DBC (byte count, bit 0 = interrupt on completion)
```

### API

- `int ahci_init(void)` — scan PCI for AHCI controller, init HBA and ports
- `int ahci_read(int port, uint64_t lba, void *buf, int count)` — synchronous read
- `int ahci_write(int port, uint64_t lba, const void *buf, int count)` — synchronous write

### Initialization Sequence

1. `pci_find(0x01, 0x06, 0x01)` → AHCI controller
2. `pci_enable_bus_mastering()`, `pci_enable_mmio()`
3. Read BAR5, mask lower bits → ABAR physical address
4. Map ABAR into kernel space via VMM (1 page for registers)
5. Set GHC.HC_Reset (bit 0), wait for reset to clear
6. Read CAP to get ports implemented mask, number of command slots
7. For each port bit set in PI:
   a. Allocate phys page for Command List (aligned 1K)
   b. Allocate phys page for Received FIS (aligned 256)
   c. Write PxCLB/PxCLBU, PxFB/PxFBU
   d. Clear PxSERR, set PxCMD.ST+CR+FRE
   e. Check PxSSTS—if device present, mark port active

### Read Sequence (polling)

1. Spin while PxCI has all slots busy (no free slot)
2. Find free slot index
3. Allocate phys page for the Command Table (or use pre-allocated pool)
4. Fill Command Header at slot `index`:
   - CFL = 5 (5 DWORDS FIS)
   - PRDTL = count (number of PRD entries)
   - W = 0 (read)
   - CTBA = physical address of Command Table
5. Fill Command FIS (REG_H2D):
   - FIS type = 0x27
   - PM_PORT = 0
   - C = 1 (command)
   - Command = 0x25 (READ DMA EXT)
   - LBA[0..5], count[0..1] (little-endian)
6. Fill PRDT entry: DBA = phys address of buf, DBC = count * 512 - 1
7. Write PxCI bit at slot index to issue command
8. Spin while PxCI bit is set (command in progress)
9. Check PxIS for errors (read PxIS to clear)

## Dependencies

- `hal/ahci.c` depends on: `hal/pci.h`, `mm/pmm.h`, `arch/x86_64/vmm.h`
- `hal/pci.c` depends on: `arch/x86_64/portio.h`
- Display via `serial_printf`

## Testing

- Boot kernel, call `ahci_init()`, print detected ports
- Read sector 0 (MBR), dump first 64 bytes via serial
- Verify with a known disk image attached to QEMU:
  ```
  qemu-system-x86_64 -cdrom build/swiftbsd.iso -drive file=disk.img,format=raw,if=none,id=drive0 -device ahci,id=ahci -drive file=disk2.img,format=raw,if=none,id=drive1 -device ide-hd,drive=drive0,bus=ahci.0
  ```
