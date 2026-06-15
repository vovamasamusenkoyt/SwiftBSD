#pragma once
#include <stdint.h>

#define PG_PRESENT  1
#define PG_WRITE    2
#define PG_USER     4
#define PG_HUGE     (1ULL << 7)
#define PG_NX       (1ULL << 63)

void vmm_init(void);
void vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_map_in_cr3(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap(uint64_t virt);
uint64_t vmm_alloc_page(void);
uint64_t vmm_read_cr3(void);
uint64_t vmm_clone_pml4(uint64_t parent_cr3);
void vmm_unmap_user(void);
void vmm_set_cr3(uint64_t cr3);
