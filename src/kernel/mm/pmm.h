#pragma once
#include <stdint.h>

void pmm_init(uint32_t mboot_info);
uint64_t page_alloc(void);
void page_free(uint64_t addr);
uint64_t pmm_free_count(void);
uint64_t pmm_total_mem(void);
