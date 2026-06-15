#include "vmm.h"
#include "kernel.h"
#include "pmm.h"

extern uint64_t page_pml4[];

uint64_t vmm_read_cr3(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

#define PAGE_SHIFT 12
#define PAGE_SIZE  4096

static uint64_t pt_lookup(uint64_t virt) {
    return (virt >> PAGE_SHIFT) & 0x1FF;
}

static uint64_t pd_lookup(uint64_t virt) {
    return (virt >> 21) & 0x1FF;
}

static uint64_t pdpt_lookup(uint64_t virt) {
    return (virt >> 30) & 0x1FF;
}

static uint64_t pml4_lookup(uint64_t virt) {
    return (virt >> 39) & 0x1FF;
}

static uint64_t *get_or_alloc_table(uint64_t *parent, int idx, int is_pd, int user) {
    if (parent[idx] & PG_PRESENT) {
        if (is_pd && (parent[idx] & PG_HUGE)) {
            uint64_t huge = parent[idx];
            uint64_t phys_base = huge & ~((1ULL << 21) - 1);
            uint64_t pt_flags = huge & 0x1FF;
            pt_flags &= ~PG_HUGE;
            if (huge & PG_NX) pt_flags |= PG_NX;

            uint64_t pt_paddr = page_alloc();
            if (!pt_paddr) return 0;
            for (int i = 0; i < 512; i++)
                ((uint64_t *)pt_paddr)[i] = 0;
            for (int i = 0; i < 512; i++)
                ((uint64_t *)pt_paddr)[i] = (phys_base + i * 0x1000) | pt_flags;

            parent[idx] = pt_paddr | PG_PRESENT | PG_WRITE | (user ? PG_USER : 0);
            return (uint64_t *)pt_paddr;
        }
        return (uint64_t *)(uintptr_t)(parent[idx] & ~0xFFF);
    }

    uint64_t paddr = page_alloc();
    if (!paddr) return 0;

    for (int i = 0; i < 512; i++)
        ((uint64_t *)paddr)[i] = 0;

    parent[idx] = paddr | PG_PRESENT | PG_WRITE | (user ? PG_USER : 0);
    return (uint64_t *)paddr;
}

void vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    int pml4_idx = pml4_lookup(virt);
    int pdpt_idx = pdpt_lookup(virt);
    int pd_idx   = pd_lookup(virt);
    int pt_idx   = pt_lookup(virt);

    int user = pml4_idx < 256;
    uint64_t *pdpt = get_or_alloc_table(page_pml4, pml4_idx, 0, user);
    uint64_t *pd   = get_or_alloc_table(pdpt, pdpt_idx, 0, user);
    uint64_t *pt   = get_or_alloc_table(pd, pd_idx, 1, user);

    if (!pt) {
        serial_puts("[vmm] map: out of memory\n");
        return;
    }

    pt[pt_idx] = (phys & ~0xFFF) | PG_PRESENT | (flags & 0xFFF);
    if (flags & PG_NX)
        pt[pt_idx] |= PG_NX;
    __asm__ volatile("invlpg (%0)" : : "r"(virt));
}

void vmm_unmap(uint64_t virt) {
    int pml4_idx = pml4_lookup(virt);
    int pdpt_idx = pdpt_lookup(virt);
    int pd_idx   = pd_lookup(virt);
    int pt_idx   = pt_lookup(virt);

    uint64_t *pdpt = (uint64_t *)(uintptr_t)(page_pml4[pml4_idx] & ~0xFFF);
    if (!(page_pml4[pml4_idx] & PG_PRESENT)) return;

    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpt[pdpt_idx] & ~0xFFF);
    if (!(pdpt[pdpt_idx] & PG_PRESENT)) return;

    uint64_t *pt = (uint64_t *)(uintptr_t)(pd[pd_idx] & ~0xFFF);
    if (!(pd[pd_idx] & PG_PRESENT)) return;

    pt[pt_idx] = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(virt));
}

void vmm_init(void) {
    serial_printf("[vmm] CR3=%x, PML4 at %x\n",
                  (unsigned)vmm_read_cr3(),
                  (unsigned)(uintptr_t)page_pml4);
}

uint64_t vmm_alloc_page(void) {
    uint64_t phys = page_alloc();
    if (!phys) return 0;

    for (uint64_t *p = (uint64_t *)phys; p < (uint64_t *)(phys + PAGE_SIZE); p++)
        *p = 0;

    return phys;
}
