#include "ahci.h"
#include "pci.h"
#include "kernel.h"
#include "vmm.h"
#include "pmm.h"

/* ------------------------------------------------------------------ */
/*  AHCI register offsets (ABAR)                                      */
/* ------------------------------------------------------------------ */
#define AHCI_CAP    0x00
#define AHCI_GHC    0x04
#define AHCI_PI     0x0C

/* GHC bits */
#define GHC_HR      (1u << 0)
#define GHC_IE      (1u << 1)
#define GHC_AE      (1u << 31)

/* Capability bits */
#define CAP_NP_MASK  0x1F
#define CAP_S64A     (1u << 31)

/* Port register offsets (port_base = 0x100 + port * 0x80) */
#define PORT_CLB     0x00
#define PORT_CLBU    0x04
#define PORT_FB      0x08
#define PORT_FBU     0x0C
#define PORT_IS      0x10
#define PORT_IE      0x14
#define PORT_CMD     0x18
#define PORT_TFD     0x20
#define PORT_SIG     0x24
#define PORT_SSTS    0x28
#define PORT_SCTL    0x2C
#define PORT_SERR    0x30
#define PORT_CI      0x38
#define PORT_SACT    0x3C

/* PxCMD bits */
#define CMD_ST       (1u << 0)
#define CMD_SUD      (1u << 1)
#define CMD_POD      (1u << 2)
#define CMD_CLO      (1u << 3)
#define CMD_FRE      (1u << 4)
#define CMD_CR       (1u << 15)
#define CMD_FR       (1u << 14)
#define CMD_MPSS     (1u << 13)
#define CMD_ICC_MASK 0xF0000000

/* PxSSTS bits */
#define SSTS_DET_MASK 0x0F
#define SSTS_DET_NONE    0
#define SSTS_DET_PRESENT 3

/* PxSERR bits */
#define SERR_ALL     0xFFFFFFFF

/* FIS types */
#define FIS_TYPE_REG_H2D  0x27

/* Command Header */
typedef volatile struct {
    uint32_t dw0;
    uint32_t dw1;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsvd[4];
} cmd_hdr_t;

/* Command Table: 128-byte header + N * 16 byte PRDT entries */
typedef struct {
    uint8_t  cfis[64];
    uint8_t  atapi[16];
    uint8_t  rsvd[48];
} cmd_table_header_t;

/* PRDT entry */
typedef volatile struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsvd;
    uint32_t dbc;
} prdt_entry_t;

/* ------------------------------------------------------------------ */
/*  Per-port state                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    volatile uint32_t *regs;
    int    present;
    int    initialized;

    uint64_t clb_phys;
    uint64_t fb_phys;
    cmd_hdr_t *clb_virt;
    uint8_t    *fb_virt;

    uint64_t ct_phys;
    cmd_table_header_t *ct_virt;

    uint64_t buf_phys;
    uint8_t  *buf_virt;
} ahci_port_t;

static volatile uint32_t *abar;
static ahci_port_t ports[AHCI_MAX_PORTS];
static int nports;
static int ahci_ok;

/* ------------------------------------------------------------------ */
/*  MMIO helpers                                                       */
/* ------------------------------------------------------------------ */
#define reg32(addr)  (*(volatile uint32_t *)(uintptr_t)(addr))

static void ahci_map_mmio(uint64_t phys, int pages) {
    for (int i = 0; i < pages; i++) {
        vmm_map(phys + i * 4096, phys + i * 4096, PG_PRESENT | PG_WRITE);
    }
    abar = (volatile uint32_t *)(uintptr_t)phys;
}

static uint32_t ahci_port_read(ahci_port_t *p, int reg) {
    return abar[(0x100 + (p - ports) * 0x80 + reg) / 4];
}

static void ahci_port_write(ahci_port_t *p, int reg, uint32_t val) {
    abar[(0x100 + (p - ports) * 0x80 + reg) / 4] = val;
}

/* ------------------------------------------------------------------ */
/*  Spin up a port                                                     */
/* ------------------------------------------------------------------ */
static int ahci_port_init(ahci_port_t *p, int idx) {
    (void)idx;

    /* Power on and spin up */
    uint32_t cmd = ahci_port_read(p, PORT_CMD);
    if (cmd & CMD_CR) {
        cmd &= ~CMD_ST;
        ahci_port_write(p, PORT_CMD, cmd);
    }
    if (cmd & CMD_FR) {
        cmd &= ~CMD_FRE;
        ahci_port_write(p, PORT_CMD, cmd);
    }

    cmd |= CMD_SUD | CMD_POD;
    ahci_port_write(p, PORT_CMD, cmd);

    /* Wait for device detection */
    for (int spin = 0; spin < 100000; spin++) {
        uint32_t ssts = ahci_port_read(p, PORT_SSTS);
        if ((ssts & SSTS_DET_MASK) == SSTS_DET_PRESENT)
            break;
    }

    uint32_t ssts = ahci_port_read(p, PORT_SSTS);
    if ((ssts & SSTS_DET_MASK) != SSTS_DET_PRESENT) {
        return 0;
    }

    /* Allocate command list (1 KB aligned, we use 4 KB anyway) */
    p->clb_phys = page_alloc();
    if (!p->clb_phys) return 0;
    p->clb_virt = (cmd_hdr_t *)(uintptr_t)p->clb_phys;
    for (int i = 0; i < 512; i++)
        ((volatile uint32_t *)p->clb_virt)[i] = 0;

    /* Allocate received FIS (256 byte aligned) */
    p->fb_phys = page_alloc();
    if (!p->fb_phys) { page_free(p->clb_phys); return 0; }
    p->fb_virt = (uint8_t *)(uintptr_t)p->fb_phys;
    for (int i = 0; i < 1024; i++)
        ((volatile uint32_t *)p->fb_virt)[i] = 0;

    /* Allocate a command table page (shared across commands) */
    p->ct_phys = page_alloc();
    if (!p->ct_phys) { page_free(p->clb_phys); page_free(p->fb_phys); return 0; }
    p->ct_virt = (cmd_table_header_t *)(uintptr_t)p->ct_phys;
    for (int i = 0; i < 128; i++)
        ((volatile uint32_t *)p->ct_virt)[i] = 0;

    /* Allocate a data buffer page for DMA */
    p->buf_phys = page_alloc();
    if (!p->buf_phys) { page_free(p->clb_phys); page_free(p->fb_phys); page_free(p->ct_phys); return 0; }
    p->buf_virt = (uint8_t *)(uintptr_t)p->buf_phys;

    /* Point the port to its command list and FIS area */
    ahci_port_write(p, PORT_CLB,  (uint32_t)(p->clb_phys & 0xFFFFFFFF));
    ahci_port_write(p, PORT_CLBU, (uint32_t)(p->clb_phys >> 32));
    ahci_port_write(p, PORT_FB,   (uint32_t)(p->fb_phys & 0xFFFFFFFF));
    ahci_port_write(p, PORT_FBU,  (uint32_t)(p->fb_phys >> 32));

    /* Clear errors */
    ahci_port_write(p, PORT_SERR, SERR_ALL);

    /* Enable FIS receive and start port */
    cmd = ahci_port_read(p, PORT_CMD);
    cmd |= CMD_FRE | CMD_ST;
    ahci_port_write(p, PORT_CMD, cmd);

    p->initialized = 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Read sectors                                                        */
/* ------------------------------------------------------------------ */
int ahci_read(int port, uint64_t lba, void *buf, int count) {
    if (!ahci_ok || port < 0 || port >= nports) return -1;
    ahci_port_t *p = &ports[port];
    if (!p->initialized) return -1;

    /* Amount of data per command */
    int total = count;
    int offset = 0;

    while (total > 0) {
        int batch = total > 16 ? 16 : total;

        /* Wait for a free command slot */
        uint32_t ci;
        int slot = -1;
        int spin;
        for (spin = 0; spin < 1000000; spin++) {
            ci = ahci_port_read(p, PORT_CI);
            if (ci == 0) { slot = 0; break; }
            if (!(ci & 1)) { slot = 0; break; }
            if (!(ci & 2)) { slot = 1; break; }
            if (!(ci & 4)) { slot = 2; break; }
        }
        if (slot < 0) {
            serial_printf("[ahci] no free cmd slot (CI=%x)\n", (unsigned)ci);
            return -1;
        }

        /* Fill command header */
        cmd_hdr_t *hdr = &p->clb_virt[slot];

        /* CFL=5 (5 DWORDS FIS), W=0 (read), PRDTL=1 */
        hdr->dw0   = (5 << 0) | (0 << 5) | (0 << 6) | (1 << 16);
        hdr->dw1   = 0;
        hdr->ctba  = (uint32_t)(p->ct_phys & 0xFFFFFFFF);
        hdr->ctbau = (uint32_t)(p->ct_phys >> 32);
        hdr->rsvd[0] = 0;
        hdr->rsvd[1] = 0;
        hdr->rsvd[2] = 0;
        hdr->rsvd[3] = 0;

        /* Fill command table: clear and build FIS */
        cmd_table_header_t *ct = p->ct_virt;
        for (int i = 0; i < (64 + 16 + 48) / 4; i++)
            ((volatile uint32_t *)ct)[i] = 0;

        uint8_t *fis = (uint8_t *)ct->cfis;
        fis[0]  = FIS_TYPE_REG_H2D;
        fis[1]  = 0x80;                /* C bit = 1 */
        fis[2]  = 0x25;                /* READ DMA EXT */
        fis[3]  = 0;                   /* features low */
        fis[4]  = (uint8_t)(lba);
        fis[5]  = (uint8_t)(lba >> 8);
        fis[6]  = (uint8_t)(lba >> 16);
        fis[7]  = 0x40;                /* device = LBA */
        fis[8]  = (uint8_t)(lba >> 24);
        fis[9]  = (uint8_t)(lba >> 32);
        fis[10] = (uint8_t)(lba >> 40);
        fis[11] = 0;                   /* features high */
        fis[12] = (uint8_t)(batch);
        fis[13] = (uint8_t)(batch >> 8);
        fis[14] = 0;
        fis[15] = 0;

        /* Fill PRDT entry */
        prdt_entry_t *prdt = (prdt_entry_t *)((uint8_t *)ct + 128);
        prdt->dba  = (uint32_t)(p->buf_phys & 0xFFFFFFFF);
        prdt->dbau = (uint32_t)(p->buf_phys >> 32);
        prdt->rsvd = 0;
        prdt->dbc  = (batch * 512 - 1);  /* DBC = byte count - 1, bit 0 is I */

        /* Issue command */
        ahci_port_write(p, PORT_CI, 1u << slot);

        /* Wait for completion */
        for (spin = 0; spin < 10000000; spin++) {
            if (!(ahci_port_read(p, PORT_CI) & (1u << slot)))
                break;
        }

        if (ahci_port_read(p, PORT_CI) & (1u << slot)) {
            serial_printf("[ahci] port %d: timeout on slot %d\n", port, slot);
            return -1;
        }

        /* Check error */
        uint32_t is = ahci_port_read(p, PORT_IS);
        if (is & 0xFFFFFFFF) {
            /* Clear interrupt status */
            ahci_port_write(p, PORT_IS, is);
        }

        uint32_t tfd = ahci_port_read(p, PORT_TFD);
        if (tfd & 0x01) {
            serial_printf("[ahci] port %d: error TFD=%x\n", port, (unsigned)tfd);
            return -1;
        }

        /* Copy data to caller buffer */
        __builtin_memcpy((uint8_t *)buf + offset, p->buf_virt, batch * 512);

        lba    += batch;
        offset += batch * 512;
        total  -= batch;
    }

    return offset;
}

/* ------------------------------------------------------------------ */
/*  Write sectors (DMA EXT)                                             */
/* ------------------------------------------------------------------ */
int ahci_write(int port, uint64_t lba, const void *buf, int count) {
    if (!ahci_ok || port < 0 || port >= nports) return -1;
    ahci_port_t *p = &ports[port];
    if (!p->initialized) return -1;

    int total = count;
    int offset = 0;

    while (total > 0) {
        int batch = total > 16 ? 16 : total;

        /* Copy data to DMA buffer */
        __builtin_memcpy(p->buf_virt, (const uint8_t *)buf + offset, batch * 512);

        int slot = -1;
        int spin;
        for (spin = 0; spin < 1000000; spin++) {
            uint32_t ci = ahci_port_read(p, PORT_CI);
            if (ci == 0) { slot = 0; break; }
            if (!(ci & 1)) { slot = 0; break; }
            if (!(ci & 2)) { slot = 1; break; }
            if (!(ci & 4)) { slot = 2; break; }
        }
        if (slot < 0) return -1;

        cmd_hdr_t *hdr = &p->clb_virt[slot];
        hdr->dw0   = (5 << 0) | (0 << 5) | (1 << 6) | (1 << 16);
        hdr->dw1   = 0;
        hdr->ctba  = (uint32_t)(p->ct_phys & 0xFFFFFFFF);
        hdr->ctbau = (uint32_t)(p->ct_phys >> 32);

        cmd_table_header_t *ct = p->ct_virt;
        for (int i = 0; i < (64 + 16 + 48) / 4; i++)
            ((volatile uint32_t *)ct)[i] = 0;

        uint8_t *fis = (uint8_t *)ct->cfis;
        fis[0]  = FIS_TYPE_REG_H2D;
        fis[1]  = 0x80;
        fis[2]  = 0x35;                /* WRITE DMA EXT */
        fis[3]  = 0;
        fis[4]  = (uint8_t)(lba);
        fis[5]  = (uint8_t)(lba >> 8);
        fis[6]  = (uint8_t)(lba >> 16);
        fis[7]  = 0x40;
        fis[8]  = (uint8_t)(lba >> 24);
        fis[9]  = (uint8_t)(lba >> 32);
        fis[10] = (uint8_t)(lba >> 40);
        fis[11] = 0;
        fis[12] = (uint8_t)(batch);
        fis[13] = (uint8_t)(batch >> 8);
        fis[14] = 0;
        fis[15] = 0;

        prdt_entry_t *prdt = (prdt_entry_t *)((uint8_t *)ct + 128);
        prdt->dba  = (uint32_t)(p->buf_phys & 0xFFFFFFFF);
        prdt->dbau = (uint32_t)(p->buf_phys >> 32);
        prdt->rsvd = 0;
        prdt->dbc  = (batch * 512 - 1);

        ahci_port_write(p, PORT_CI, 1u << slot);

        for (spin = 0; spin < 10000000; spin++) {
            if (!(ahci_port_read(p, PORT_CI) & (1u << slot)))
                break;
        }

        if (ahci_port_read(p, PORT_CI) & (1u << slot)) return -1;

        uint32_t tfd = ahci_port_read(p, PORT_TFD);
        if (tfd & 0x01) return -1;

        lba    += batch;
        offset += batch * 512;
        total  -= batch;
    }

    return offset;
}

/* ------------------------------------------------------------------ */
/*  Init                                                                */
/* ------------------------------------------------------------------ */
int ahci_init(void) {
    pci_dev_t *dev = pci_find(0x01, 0x06, 0x01);
    if (!dev) {
        log_warn("ahci: no AHCI controller found");
        return 0;
    }

    log_info("ahci: AHCI controller at %x:%x.%d",
             (unsigned)dev->bus, (unsigned)dev->slot, (unsigned)dev->func);

    uint64_t abar_phys = dev->bar[5];
    if (abar_phys & 1) {
        log_fail("ahci: BAR5 is not memory-mapped");
        return 0;
    }

    abar_phys &= ~0xF;

    pci_enable_bus_master(dev);
    pci_enable_mmio(dev);

    /* Map ABAR: we need about 0x1100 bytes for 32 ports */
    ahci_map_mmio(abar_phys, 8);

    /* Enable AHCI mode */
    reg32(abar + AHCI_GHC / 4) |= GHC_AE;

    /* Read ports implemented */
    uint32_t pi = reg32(abar + AHCI_PI / 4);
    uint32_t cap = reg32(abar + AHCI_CAP / 4);

    nports = (cap & CAP_NP_MASK) + 1;

    int active = 0;
    for (int i = 0; i < AHCI_MAX_PORTS && i < nports; i++) {
        if (!(pi & (1u << i))) continue;
        ports[i].regs = &abar[(0x100 + i * 0x80) / 4];
        ports[i].present = 1;
        if (ahci_port_init(&ports[i], i)) {
            log_info("ahci: port %d registered", i);
            active++;
        }
    }
    if (active)
        log_info("ahci: %d active port(s)", active);

    ahci_ok = 1;
    return 1;
}

int ahci_port_count(void) { return nports; }
