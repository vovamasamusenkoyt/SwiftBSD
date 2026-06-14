#pragma once
#include <stdint.h>

#define PG_PRESENT  1
#define PG_WRITE    2
#define PG_USER     4
#define PG_HUGE     (1ULL << 7)
#define PG_NX       (1ULL << 63)

void vmm_init(void);
void vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap(uint64_t virt);
uint64_t vmm_alloc_page(void);
