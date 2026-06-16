#ifndef USER_MALLOC_H
#define USER_MALLOC_H

/*
 * Simple segregated-free-list malloc using brk().
 *
 * Buckets: 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
 * Larger allocations are page-aligned via brk.
 */

#define MALLOC_BUCKETS 9
#define MALLOC_MIN     16
#define MALLOC_MAX     4096

struct malloc_hdr {
    unsigned short  magic;
    unsigned short  bucket;
    struct malloc_hdr *next;
};

static struct malloc_hdr *free_lists[MALLOC_BUCKETS];

static const int bucket_sz[MALLOC_BUCKETS] = {
    16, 32, 64, 128, 256, 512, 1024, 2048, 4096
};

static int malloc_bucket(unsigned long size) {
    if (size <= 16) return 0;
    unsigned long req = size + sizeof(struct malloc_hdr);
    for (int i = 0; i < MALLOC_BUCKETS; i++)
        if (bucket_sz[i] >= req) return i;
    return -1;
}

static void malloc_add_page(int b) {
    long sz = bucket_sz[b];
    long page_avail = 4096 / sz;
    long needed = page_avail > 0 ? page_avail : 1;
    long alloc = needed * sz;
    long addr = (long)sbrk(0);
    long page_end = (addr + alloc + 0xFFF) & ~0xFFF;
    long cur = (long)sbrk(0);
    if (page_end > cur) {
        long inc = page_end - cur;
        long ret = (long)sbrk(inc);
        if (ret == -1) return;
    }
    for (long i = 0; i < needed; i++) {
        struct malloc_hdr *h = (struct malloc_hdr *)(addr + i * sz);
        h->magic  = 0xBEEF;
        h->bucket = b;
        h->next   = free_lists[b];
        free_lists[b] = h;
    }
}

static void *malloc(unsigned long size) {
    if (size == 0) return 0;
    if (size > MALLOC_MAX) {
        long aligned = (size + 0xFFF) & ~0xFFF;
        long cur = (long)sbrk(0);
        if ((long)sbrk(aligned) == -1) return 0;
        return (void *)cur;
    }
    int b = malloc_bucket(size);
    if (b < 0) return 0;
    if (!free_lists[b])
        malloc_add_page(b);
    if (!free_lists[b]) return 0;
    struct malloc_hdr *h = free_lists[b];
    free_lists[b] = h->next;
    h->magic = 0xBEEF;
    return (void *)(h + 1);
}

static void free(void *ptr) {
    if (!ptr) return;
    struct malloc_hdr *h = ((struct malloc_hdr *)ptr) - 1;
    if (h->magic != 0xBEEF) return;
    int b = h->bucket;
    if (b < 0 || b >= MALLOC_BUCKETS) return;
    h->next = free_lists[b];
    free_lists[b] = h;
}

static void *calloc(unsigned long nmemb, unsigned long size) {
    unsigned long total = nmemb * size;
    void *p = malloc(total);
    if (p) {
        unsigned char *cp = (unsigned char *)p;
        for (unsigned long i = 0; i < total; i++) cp[i] = 0;
    }
    return p;
}

static void *realloc(void *ptr, unsigned long new_size) {
    if (!ptr) return malloc(new_size);
    if (new_size == 0) { free(ptr); return 0; }
    struct malloc_hdr *h = ((struct malloc_hdr *)ptr) - 1;
    unsigned long old_size;
    if (h->bucket >= 0 && h->bucket < MALLOC_BUCKETS)
        old_size = bucket_sz[h->bucket] - sizeof(struct malloc_hdr);
    else
        old_size = 0;
    void *newp = malloc(new_size);
    if (!newp) return 0;
    unsigned long copy = old_size < new_size ? old_size : new_size;
    unsigned char *src = (unsigned char *)ptr;
    unsigned char *dst = (unsigned char *)newp;
    for (unsigned long i = 0; i < copy; i++) dst[i] = src[i];
    free(ptr);
    return newp;
}

#endif
