#include "kernel.h"
#include "sched.h"
#include "vfs.h"
#include "swiftfs2.h"
#include "elf.h"
#include "string.h"
#include "pmm.h"
#include "vmm.h"

#define SC_PUTS  0
#define SC_NOP   1
#define SC_YIELD 2
#define SC_OPEN  3
#define SC_READ  4
#define SC_WRITE 5
#define SC_CLOSE 6
#define SC_EXEC  7
#define SC_EXIT  8
#define SC_FSTAT 9
#define SC_HALT  10
#define SC_MEMINFO 11
#define SC_FORK  12
#define SC_WAIT  13
#define SC_GETPID 14
#define SC_BRK   15
#define SC_MMAP  16
#define SC_MUNMAP 17
#define SC_MSYNC 18
#define SC_PIPE  19
#define SC_DUP   20
#define SC_DUP2  21
#define SC_MPROTECT 22

#define MAP_FAILED ((void *)-1)
#define ARGS_PAGE ((char *)0x7F004000)

extern uint64_t page_alloc(void);
extern void vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);

/* ── VMA helpers ── */

static int vma_alloc_slot(void) {
    struct vma *v = procs[current_idx].vmas;
    for (int i = 0; i < VMA_MAX; i++)
        if (!v[i].used) return i;
    return -1;
}

static int vma_find(uint64_t addr) {
    struct vma *v = procs[current_idx].vmas;
    for (int i = 0; i < VMA_MAX; i++)
        if (v[i].used && addr >= v[i].start && addr < v[i].end)
            return i;
    return -1;
}

static uint64_t vma_hint(void) {
    uint64_t hint = 0x40000000;
    struct vma *v = procs[current_idx].vmas;
    for (int i = 0; i < VMA_MAX; i++)
        if (v[i].used && v[i].end > hint)
            hint = v[i].end;
    return (hint + 0xFFF) & ~0xFFF;
}

static int vma_split(int idx, uint64_t split_addr) {
    struct vma *v = procs[current_idx].vmas;
    if (split_addr <= v[idx].start || split_addr >= v[idx].end)
        return -1;
    int right = vma_alloc_slot();
    if (right < 0) return -1;
    v[right] = v[idx];
    v[right].start = split_addr;
    v[right].foff += split_addr - v[idx].start;
    v[idx].end = split_addr;
    procs[current_idx].vma_count++;
    return right;
}

static int vma_compat(struct vma *a, struct vma *b) {
    if (a->prot != b->prot) return 0;
    if (a->flags != b->flags) return 0;
    if (a->fd != b->fd) return 0;
    if (a->fd >= 0) {
        uint64_t expected = a->foff + (a->end - a->start);
        if (b->foff != expected) return 0;
    }
    return 1;
}

static void vma_try_merge(int idx) {
    struct vma *v = procs[current_idx].vmas;
    for (int dir = -1; dir <= 1; dir += 2) {
        int other = idx + dir;
        if (other < 0 || other >= VMA_MAX || !v[other].used)
            continue;
        struct vma *a, *b;
        if (dir == -1) { a = &v[other]; b = &v[idx]; }
        else           { a = &v[idx];   b = &v[other]; }
        if (a->end == b->start && vma_compat(a, b)) {
            a->end = b->end;
            b->used = 0;
            procs[current_idx].vma_count--;
            return;
        }
    }
}

static void vma_writeback_dirty(uint64_t start, uint64_t end, struct vma *v) {
    if (v->fd < 0 || !(v->flags & 0x01))
        return;
    for (uint64_t p = start; p < end; p += 0x1000) {
        uint64_t *pt = vmm_pt_lookup(p);
        if (pt && (*pt & PG_DIRTY)) {
            uint64_t foff = v->foff + (p - v->start);
            vfs_lseek(v->fd, (int64_t)foff, SEEK_SET);
            char tmp[4096];
            memcpy(tmp, (void *)(uintptr_t)p, 4096);
            vfs_write(v->fd, tmp, 4096);
            *pt &= ~PG_DIRTY;
        }
    }
}

static void vma_unmap_pages(uint64_t start, uint64_t end) {
    for (uint64_t p = start; p < end; p += 0x1000)
        vmm_unmap(p);
}

/* ── syscall handler ── */

uint64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    switch (num) {
    case SC_PUTS:
        serial_puts((const char *)arg1);
        break;
    case SC_NOP:
        break;
    case SC_YIELD:
        sched_yield();
        break;
    case SC_OPEN:
        return (uint64_t)vfs_open((const char *)arg1, (int)arg2);
    case SC_READ:
        return (uint64_t)vfs_read((int)arg1, (void *)arg2, (uint32_t)arg3);
    case SC_WRITE:
        return (uint64_t)vfs_write((int)arg1, (const void *)arg2, (uint32_t)arg3);
    case SC_CLOSE:
        return (uint64_t)vfs_close((int)arg1);
    case SC_EXEC:
        {
            char args_buf[256];
            if (arg2) {
                const char *src = (const char *)arg2;
                int i;
                for (i = 0; i < 255 && src[i]; i++) args_buf[i] = src[i];
                args_buf[i] = 0;
            } else {
                args_buf[0] = 0;
            }

            int fd = vfs_open((const char *)arg1, O_RDONLY);
            if (fd < 0) return -1;

            uint32_t size = 0, cap = 65536;
            uint8_t *file_data = kmalloc(cap);
            if (!file_data) { vfs_close(fd); return -1; }

            uint8_t tmp[512];
            int n;
            while ((n = vfs_read(fd, tmp, sizeof(tmp))) > 0) {
                if (size + n > cap) {
                    cap *= 2;
                    uint8_t *np = kmalloc(cap);
                    memcpy(np, file_data, size);
                    kfree(file_data);
                    file_data = np;
                }
                memcpy(file_data + size, tmp, n);
                size += n;
            }
            vfs_close(fd);
            if (size == 0) { kfree(file_data); return -1; }

            if (size >= 64) {
                uint64_t vmagic = *(uint32_t *)file_data;
                uint64_t ventry = *(uint64_t *)&file_data[0x18];
                serial_printf("[exec] file=%s size=%u magic=%x entry=%x\n",
                              (const char *)arg1, (unsigned)size,
                              (unsigned)vmagic, (unsigned)ventry);
            }

            uint64_t entry;
            if (elf64_load(file_data, &entry) < 0) {
                serial_printf("[exec] elf64_load FAILED for %s size=%u\n",
                              (const char *)arg1, (unsigned)size);
                kfree(file_data);
                return -1;
            }
            kfree(file_data);

            int i;
            for (i = 0; args_buf[i]; i++) ARGS_PAGE[i] = args_buf[i];
            ARGS_PAGE[i] = 0;

            serial_printf("[exec] entry=%x rsp=%x\n",
                          (unsigned)entry, (unsigned)0x7F003000);
            extern void user_entry(uint64_t entry, uint64_t rsp);
            if (entry == 0) {
                serial_puts("[exec] ERROR: entry is 0!\n");
                for (;;) __asm__("hlt");
            }
            user_entry(entry, 0x7F003000);
            return 0;
        }
    case SC_EXIT:
        serial_printf("[user] PID %d exit\n", procs[current_idx].pid);
        proc_exit((int)arg1);
        __builtin_unreachable();
    case SC_FSTAT:
        {
            struct stat st;
            int ret = vfs_fstat((int)arg1, &st);
            if (ret == 0 && arg2) {
                struct stat *u = (struct stat *)arg2;
                *u = st;
            }
            return (uint64_t)ret;
        }
    case SC_HALT:
        serial_puts("[user] halt\n");
        for (;;) __asm__("hlt");
    case SC_MEMINFO:
        {
            uint64_t *buf = (uint64_t *)arg1;
            buf[0] = pmm_total_mem();
            buf[1] = pmm_free_count();
        }
        break;
    case SC_FORK:
        return (uint64_t)proc_fork();
    case SC_WAIT:
        {
            int code;
            int ret = proc_wait(&code);
            if (ret > 0 && arg1) {
                int *uptr = (int *)arg1;
                *uptr = code;
            }
            return (uint64_t)ret;
        }
    case SC_GETPID:
        return (uint64_t)procs[current_idx].pid;
    case SC_BRK:
        {
            uint64_t new_brk = arg1;
            uint64_t *heap_break = &procs[current_idx].heap_break;
            if (new_brk == 0)
                return *heap_break;
            if (new_brk < procs[current_idx].heap_start) {
                return -1;
            }
            uint64_t old_page = *heap_break & ~0xFFF;
            uint64_t new_page = (new_brk + 0xFFF) & ~0xFFF;
            if (new_page > old_page) {
                for (uint64_t v = old_page; v < new_page; v += 0x1000) {
                    uint64_t phys = page_alloc();
                    if (!phys) return -1;
                    vmm_map(v, phys, PG_PRESENT | PG_WRITE | PG_USER | PG_NX);
                }
            } else if (new_page < old_page) {
                for (uint64_t v = new_page; v < old_page; v += 0x1000)
                    vmm_unmap(v);
            }
            *heap_break = new_brk;
            return new_brk;
        }
    case SC_MMAP:
        {
            uint64_t addr   = arg1;
            uint64_t length = arg2;
            int prot        = (int)arg3;

            struct { int flags; int fd; int pad; int64_t offset; } *mmap_args;
            mmap_args = (void *)&arg3;
            int flags      = mmap_args->flags;
            int fd         = mmap_args->fd;
            int64_t offset = mmap_args->offset;

            if (length == 0) return (uint64_t)MAP_FAILED;
            length = (length + 0xFFF) & ~0xFFF;

            int vma_idx = vma_alloc_slot();
            if (vma_idx < 0) return (uint64_t)MAP_FAILED;

            if (addr == 0) addr = vma_hint();

            struct vma *v = &procs[current_idx].vmas[vma_idx];
            v->start = addr;
            v->end   = addr + length;
            v->prot  = prot;
            v->flags = flags;
            v->fd    = fd;
            v->foff  = (uint64_t)(fd >= 0 ? offset : 0);
            v->used  = 1;
            procs[current_idx].vma_count++;
            return addr;
        }
    case SC_MUNMAP:
        {
            uint64_t addr   = arg1;
            uint64_t length = arg2;
            if (length == 0) return 0;
            uint64_t end = addr + ((length + 0xFFF) & ~0xFFF);

            for (int i = 0; i < VMA_MAX; i++) {
                struct vma *v = &procs[current_idx].vmas[i];
                if (!v->used || addr >= v->end || end <= v->start)
                    continue;

                if (addr > v->start) {
                    int r = vma_split(i, addr);
                    if (r < 0) return -1;
                    v = &procs[current_idx].vmas[r];
                    i = r;
                }

                if (end < v->end) {
                    vma_split(i, end);
                }

                v = &procs[current_idx].vmas[i];
                vma_writeback_dirty(v->start, v->end, v);
                vma_unmap_pages(v->start, v->end);
                v->used = 0;
                procs[current_idx].vma_count--;
                vma_try_merge(i);
            }
            return 0;
        }
    case SC_MSYNC:
        {
            uint64_t addr   = arg1;
            uint64_t length = arg2;
            if (length == 0) return 0;
            uint64_t end = addr + ((length + 0xFFF) & ~0xFFF);

            for (int i = 0; i < VMA_MAX; i++) {
                struct vma *v = &procs[current_idx].vmas[i];
                if (!v->used || addr >= v->end || end <= v->start)
                    continue;
                uint64_t start = addr > v->start ? addr : v->start;
                uint64_t stop  = end < v->end ? end : v->end;
                vma_writeback_dirty(start, stop, v);
            }
            return 0;
        }
    case SC_PIPE:
        {
            int fds[2];
            int ret = vfs_pipe(fds);
            if (ret == 0 && arg1) {
                int *uptr = (int *)arg1;
                uptr[0] = fds[0];
                uptr[1] = fds[1];
            }
            return (uint64_t)ret;
        }
    case SC_DUP:
        return (uint64_t)vfs_dup((int)arg1);
    case SC_DUP2:
        return (uint64_t)vfs_dup2((int)arg1, (int)arg2);
    case SC_MPROTECT:
        {
            uint64_t addr   = arg1;
            uint64_t length = arg2;
            int prot        = (int)arg3;
            if (length == 0) return 0;
            uint64_t end = addr + ((length + 0xFFF) & ~0xFFF);

            for (int i = 0; i < VMA_MAX; i++) {
                struct vma *v = &procs[current_idx].vmas[i];
                if (!v->used || addr >= v->end || end <= v->start)
                    continue;

                if (addr > v->start) {
                    int r = vma_split(i, addr);
                    if (r < 0) return -1;
                    v = &procs[current_idx].vmas[r];
                    i = r;
                }

                if (end < v->end) {
                    vma_split(i, end);
                }

                v = &procs[current_idx].vmas[i];
                v->prot = prot;

                for (uint64_t p = v->start; p < v->end; p += 0x1000) {
                    uint64_t *pt = vmm_pt_lookup(p);
                    if (pt && (*pt & PG_PRESENT)) {
                        uint64_t paddr = *pt & 0x0000FFFFFFFFF000ULL;
                        uint64_t keep = *pt & (PG_ACCESSED | PG_DIRTY);
                        uint64_t new_flags = PG_PRESENT | PG_USER | keep;
                        if (prot & 2) new_flags |= PG_WRITE;
                        if (!(prot & 4)) new_flags |= PG_NX;
                        *pt = paddr | new_flags;
                    }
                }
                vma_try_merge(i);
            }
            return 0;
        }
    }
    return 0;
}
