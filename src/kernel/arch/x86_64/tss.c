#include "tss.h"
#include "kernel.h"
#include "pmm.h"
#include "vmm.h"

extern uint64_t page_pml4[];

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint32_t reserved2;
    uint32_t reserved3;
    uint16_t reserved4;
    uint16_t iopb_offset;
} __attribute__((packed));

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit_high;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_tss_descriptor {
    struct gdt_entry low;
    uint32_t base_high32;
    uint32_t reserved;
} __attribute__((packed));

static struct tss *tss;

void tss_init(void) {
    uint64_t phys = page_alloc();
    if (!phys) {
        serial_puts("[tss] OOM\n");
        for (;;) __asm__("hlt");
    }

    tss = (struct tss *)phys;
    for (uint64_t *p = (uint64_t *)tss; p < (uint64_t *)(tss + 1); p++)
        *p = 0;
    tss->iopb_offset = sizeof(struct tss);

    uint64_t stack = page_alloc();
    if (stack) {
        tss->rsp[0] = stack + 4096;
        tss->ist[1] = stack + 4096;
    } else {
        serial_puts("[tss] stack OOM\n");
        for (;;) __asm__("hlt");
    }

    uint64_t tss_paddr = (uint64_t)tss;

    uint8_t gdt_raw[10];
    __asm__ volatile("sgdt %0" : "=m"(gdt_raw) : : "memory");

    uint16_t gdt_limit = *(uint16_t *)&gdt_raw[0];
    uint64_t gdt_base  = *(uint32_t *)&gdt_raw[2];

    serial_printf("[tss] GDT base=%x limit=%x\n",
                  (unsigned)(gdt_base & 0xFFFFFFFF),
                  (unsigned)gdt_limit);

    struct gdt_tss_descriptor *tss_entry = (struct gdt_tss_descriptor *)(gdt_base + 0x28);

    tss_entry->low.limit_low = sizeof(struct tss) - 1;
    tss_entry->low.base_low  = tss_paddr & 0xFFFF;
    tss_entry->low.base_mid  = (tss_paddr >> 16) & 0xFF;
    tss_entry->low.access    = 0x89;
    tss_entry->low.flags_limit_high = 0;
    tss_entry->low.base_high = (tss_paddr >> 24) & 0xFF;
    tss_entry->base_high32   = (tss_paddr >> 32) & 0xFFFFFFFF;
    tss_entry->reserved      = 0;

    uint16_t tr = 0x28;
    __asm__ volatile("ltr %0" : : "r"(tr));

    serial_printf("[tss] TSS at %x, stack %x\n",
                  (unsigned)tss_paddr, (unsigned)stack);
}

void tss_set_kernel_stack(uint64_t rsp0) {
    if (tss)
        tss->rsp[0] = rsp0;
}
