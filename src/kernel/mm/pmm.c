#include "kernel.h"

#define PAGE_SIZE 4096
#define MAX_PAGES (512 * 1024 * 1024 / PAGE_SIZE)

extern uint8_t _bss_end[];

static uint8_t bitmap[MAX_PAGES / 8];
static uint64_t last_phys_page;
static uint64_t free_page_count;
static uint64_t total_mem;

static void bitmap_set(uint64_t p) {
    bitmap[p / 8] |= (1 << (p % 8));
}

static void bitmap_clear(uint64_t p) {
    bitmap[p / 8] &= ~(1 << (p % 8));
}

static int bitmap_test(uint64_t p) {
    return (bitmap[p / 8] >> (p % 8)) & 1;
}

void pmm_init(uint32_t mboot_info) {
    last_phys_page = 0;
    free_page_count = 0;
    total_mem = 0;

    for (uint64_t i = 0; i < MAX_PAGES / 8; i++)
        bitmap[i] = 0xFF;

    uintptr_t addr = mboot_info + 8;
    for (;;) {
        struct multiboot_tag {
            uint32_t type;
            uint32_t size;
        } *tag = (struct multiboot_tag *)addr;

        if (tag->type == 0)
            break;

        if (tag->type == 6) {
            struct {
                uint32_t type;
                uint32_t size;
                uint32_t entry_size;
                uint32_t entry_version;
                struct {
                    uint64_t addr;
                    uint64_t len;
                    uint32_t type;
                    uint32_t zero;
                } entries[];
            } *mmap = (void *)tag;

            int n = (mmap->size - sizeof(*mmap)) / mmap->entry_size;
            for (int i = 0; i < n; i++) {
                uint64_t start = mmap->entries[i].addr;
                uint64_t end = start + mmap->entries[i].len;

                if (mmap->entries[i].type == 1) {
                    total_mem += mmap->entries[i].len;
                    uint64_t ps = (start + PAGE_SIZE - 1) / PAGE_SIZE;
                    uint64_t pe = end / PAGE_SIZE;

                    if (pe > MAX_PAGES) pe = MAX_PAGES;

                    for (uint64_t p = ps; p < pe; p++) {
                        bitmap_clear(p);
                        free_page_count++;
                    }

                    if (pe > last_phys_page)
                        last_phys_page = pe;
                }
            }
        }

        addr += (tag->size + 7) & ~7;
    }

    uint64_t bss_end_page = (uint64_t)_bss_end / PAGE_SIZE;
    for (uint64_t i = 0; i <= bss_end_page; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_page_count--;
        }
    }

    serial_printf("[pmm] %d pages free, last PF %d\n",
                  (unsigned int)free_page_count,
                  (unsigned int)last_phys_page);
}

uint64_t page_alloc(void) {
    for (uint64_t i = 0; i < last_phys_page; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_page_count--;
            return i * PAGE_SIZE;
        }
    }
    serial_puts("[pmm] OUT OF MEMORY\n");
    return 0;
}

void page_free(uint64_t addr) {
    uint64_t p = addr / PAGE_SIZE;
    if (p >= MAX_PAGES) return;
    if (bitmap_test(p)) {
        bitmap_clear(p);
        free_page_count++;
    }
}

uint64_t pmm_free_count(void) {
    return free_page_count;
}

uint64_t pmm_total_mem(void) {
    return total_mem;
}
