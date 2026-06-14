#include "kernel.h"

#define HEAP_SIZE (1024 * 1024)
#define ALIGN 8

static char heap_pool[HEAP_SIZE];

struct heap_block {
    size_t size;
    int free;
    struct heap_block *next;
};

static struct heap_block *heap_head;

void kheap_init(void) {
    heap_head = (struct heap_block *)heap_pool;
    heap_head->size = HEAP_SIZE - sizeof(struct heap_block);
    heap_head->free = 1;
    heap_head->next = 0;

    serial_printf("[kheap] pool at %x, %d bytes\n",
                  (unsigned)(uintptr_t)heap_pool, HEAP_SIZE);
}

void *kmalloc(size_t size) {
    size = (size + ALIGN - 1) & ~(ALIGN - 1);

    struct heap_block *b = heap_head;
    while (b) {
        if (b->free && b->size >= size) {
            if (b->size > size + sizeof(struct heap_block)) {
                struct heap_block *new_b = (struct heap_block *)((uint8_t *)(b + 1) + size);
                new_b->size = b->size - size - sizeof(struct heap_block);
                new_b->free = 1;
                new_b->next = b->next;
                b->size = size;
                b->next = new_b;
            }
            b->free = 0;
            return (void *)(b + 1);
        }
        b = b->next;
    }

    return 0;
}

void kfree(void *ptr) {
    if (!ptr) return;
    struct heap_block *b = (struct heap_block *)ptr - 1;
    b->free = 1;

    struct heap_block *cur = heap_head;
    while (cur) {
        if (cur->free && cur->next && cur->next->free) {
            cur->size += sizeof(struct heap_block) + cur->next->size;
            cur->next = cur->next->next;
        }
        cur = cur->next;
    }
}
