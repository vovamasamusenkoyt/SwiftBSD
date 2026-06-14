#include "kernel.h"

#define MULTIBOOT_TAG_TYPE_MMAP 6

struct multiboot_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct multiboot_mmap_entry {
        uint64_t addr;
        uint64_t len;
        uint32_t type;
        uint32_t zero;
    } entries[];
};

static uint64_t total_mem = 0;

void pmem_init(uint32_t mboot_info) {
    uintptr_t addr = mboot_info + 8;
    for (;;) {
        struct multiboot_tag_mmap *tag = (struct multiboot_tag_mmap *)addr;
        if (tag->type == 0)
            break;
        if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
            int n = (tag->size - sizeof(struct multiboot_tag_mmap)) / tag->entry_size;
            for (int i = 0; i < n; i++) {
                if (tag->entries[i].type == 1)
                    total_mem += tag->entries[i].len;
            }
        }
        addr += (tag->size + 7) & ~7;
    }
}

uint64_t pmem_total(void) {
    return total_mem;
}
