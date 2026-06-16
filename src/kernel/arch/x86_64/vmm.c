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

static uint64_t *phys_to_virt(uint64_t phys) {
    return (uint64_t *)(uintptr_t)phys;
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

void vmm_map_in_cr3(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags) {
    int pml4_idx = pml4_lookup(virt);
    int pdpt_idx = pdpt_lookup(virt);
    int pd_idx   = pd_lookup(virt);
    int pt_idx   = pt_lookup(virt);
    int user = pml4_idx < 256;

    uint64_t *pml4 = phys_to_virt(cr3);
    uint64_t *pdpt = get_or_alloc_table(pml4, pml4_idx, 0, user);
    uint64_t *pd   = get_or_alloc_table(pdpt, pdpt_idx, 0, user);
    uint64_t *pt   = get_or_alloc_table(pd, pd_idx, 1, user);

    if (!pt) {
        log_fail("vmm: map OOM");
        return;
    }

    pt[pt_idx] = (phys & ~0xFFF) | PG_PRESENT | (flags & 0xFFF);
    if (flags & PG_NX)
        pt[pt_idx] |= PG_NX;
    if (cr3 == vmm_read_cr3())
        __asm__ volatile("invlpg (%0)" : : "r"(virt));
}

void vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    vmm_map_in_cr3(vmm_read_cr3(), virt, phys, flags);
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

    uint64_t pte = pt[pt_idx];
    if (pte & PG_USER)
        page_free(pte & ~0xFFF);

    pt[pt_idx] = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(virt));
}

void vmm_init(void) {
    log_info("vmm: CR3=0x%x", (unsigned)vmm_read_cr3());
    log_info("vmm: PML4 at 0x%x", (unsigned)(uintptr_t)page_pml4);
}

uint64_t vmm_alloc_page(void) {
    uint64_t phys = page_alloc();
    if (!phys) return 0;

    for (uint64_t *p = (uint64_t *)phys; p < (uint64_t *)(phys + PAGE_SIZE); p++)
        *p = 0;

    return phys;
}

static uint64_t clone_table_rw(uint64_t parent_phys, int user) {
    uint64_t *parent = phys_to_virt(parent_phys);
    uint64_t child_phys = vmm_alloc_page();
    if (!child_phys) return 0;
    uint64_t *child = phys_to_virt(child_phys);
    uint64_t flags = PG_PRESENT | PG_WRITE;
    if (user) flags |= PG_USER;

    for (int i = 0; i < 512; i++) {
        uint64_t e = parent[i];
        if (!(e & PG_PRESENT)) { child[i] = 0; continue; }
        if (e & PG_HUGE) {
            child[i] = e;
            continue;
        }
        child[i] = (e & ~0xFFF) | flags;
    }
    return child_phys;
}

static uint64_t clone_pt_cow(uint64_t parent_phys) {
    uint64_t *parent = phys_to_virt(parent_phys);
    uint64_t child_phys = vmm_alloc_page();
    if (!child_phys) return 0;
    uint64_t *child = phys_to_virt(child_phys);

    for (int i = 0; i < 512; i++) {
        uint64_t e = parent[i];
        if (!(e & PG_PRESENT)) { child[i] = 0; continue; }
        if (e & PG_USER) {
            e &= ~(uint64_t)PG_WRITE;
            parent[i] = e;
        }
        child[i] = e;
    }
    return child_phys;
}

uint64_t vmm_clone_pml4(uint64_t parent_cr3) {
    uint64_t *parent = phys_to_virt(parent_cr3);
    uint64_t child_cr3 = vmm_alloc_page();
    if (!child_cr3) return 0;
    uint64_t *child = phys_to_virt(child_cr3);

    for (int i = 256; i < 512; i++)
        child[i] = parent[i];

    for (int i = 0; i < 256; i++) {
        uint64_t e = parent[i];
        if (!(e & PG_PRESENT)) { child[i] = 0; continue; }
        if (e & PG_HUGE) {
            child[i] = e;
            continue;
        }
        uint64_t child_pdpt = clone_table_rw(e & ~0xFFF, 1);
        if (!child_pdpt) return 0;
        child[i] = (child_pdpt & ~0xFFF) | PG_PRESENT | PG_WRITE | PG_USER;

        uint64_t *parent_pdpt = phys_to_virt(e & ~0xFFF);
        uint64_t *child_pdpt_v = phys_to_virt(child_pdpt);
        for (int j = 0; j < 512; j++) {
            uint64_t pe = parent_pdpt[j];
            if (!(pe & PG_PRESENT)) { child_pdpt_v[j] = 0; continue; }
            if (pe & PG_HUGE) {
                child_pdpt_v[j] = pe;
                continue;
            }
            uint64_t child_pd = clone_table_rw(pe & ~0xFFF, 1);
            if (!child_pd) return 0;
            child_pdpt_v[j] = (child_pd & ~0xFFF) | PG_PRESENT | PG_WRITE | PG_USER;

            uint64_t *parent_pd = phys_to_virt(pe & ~0xFFF);
            uint64_t *child_pd_v = phys_to_virt(child_pd);
            for (int k = 0; k < 512; k++) {
                uint64_t pde = parent_pd[k];
                if (!(pde & PG_PRESENT)) { child_pd_v[k] = 0; continue; }
                if (pde & PG_HUGE) {
                    child_pd_v[k] = pde;
                    continue;
                }
                uint64_t child_pt = clone_pt_cow(pde & ~0xFFF);
                if (!child_pt) return 0;
                child_pd_v[k] = (child_pt & ~0xFFF) | PG_PRESENT | PG_WRITE | PG_USER;

                uint64_t *parent_pt = phys_to_virt(pde & ~0xFFF);
                uint64_t *child_pt_v = phys_to_virt(child_pt);
                for (int l = 0; l < 512; l++) {
                    uint64_t pte = parent_pt[l];
                    if (!(pte & PG_PRESENT)) { child_pt_v[l] = 0; continue; }
                    if (pte & PG_USER) {
                        pte &= ~(uint64_t)PG_WRITE;
                        parent_pt[l] = pte;
                    }
                    child_pt_v[l] = pte;
                }
            }
        }
    }
    return child_cr3;
}

void vmm_unmap_user(void) {
    uint64_t *pml4 = phys_to_virt(vmm_read_cr3());
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & PG_PRESENT)) continue;
        uint64_t *pdpt = phys_to_virt(pml4[i] & ~0xFFF);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PG_PRESENT)) continue;
            uint64_t *pd = phys_to_virt(pdpt[j] & ~0xFFF);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PG_PRESENT)) continue;
                uint64_t *pt = phys_to_virt(pd[k] & ~0xFFF);
                for (int l = 0; l < 512; l++) {
                    if (!(pt[l] & PG_PRESENT)) continue;
                    if (pt[l] & PG_USER) {
                        page_free(pt[l] & ~0xFFF);
                    }
                }
                page_free(pd[k] & ~0xFFF);
            }
            page_free(pdpt[j] & ~0xFFF);
        }
        page_free(pml4[i] & ~0xFFF);
        pml4[i] = 0;
    }
}

void vmm_set_cr3(uint64_t cr3) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

uint64_t *vmm_pt_lookup(uint64_t virt) {
    int pml4_idx = pml4_lookup(virt);
    int pdpt_idx = pdpt_lookup(virt);
    int pd_idx   = pd_lookup(virt);
    int pt_idx   = pt_lookup(virt);

    if (!(page_pml4[pml4_idx] & PG_PRESENT)) return 0;
    uint64_t *pdpt = phys_to_virt(page_pml4[pml4_idx] & ~0xFFF);

    if (!(pdpt[pdpt_idx] & PG_PRESENT)) return 0;
    uint64_t *pd = phys_to_virt(pdpt[pdpt_idx] & ~0xFFF);

    if (!(pd[pd_idx] & PG_PRESENT)) return 0;
    uint64_t *pt = phys_to_virt(pd[pd_idx] & ~0xFFF);

    if (!(pt[pt_idx] & PG_PRESENT)) return 0;
    return &pt[pt_idx];
}
