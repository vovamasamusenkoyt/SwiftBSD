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

#define MAP_FAILED ((void *)-1)
#define ARGS_PAGE ((char *)0x7F004000)

extern uint64_t page_alloc(void);
extern void vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);

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

            /* Write args to user args page */
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
            (void)prot;

            /* arg4 = flags packed in high bits of arg3 */
            struct { int flags; int fd; int pad; int64_t offset; } *mmap_args;
            mmap_args = (void *)&arg3;
            int flags      = mmap_args->flags;
            int fd         = mmap_args->fd;
            int64_t offset = mmap_args->offset;

            if (length == 0) return (uint64_t)MAP_FAILED;
            length = (length + 0xFFF) & ~0xFFF;

            int vma_idx = -1;
            for (int i = 0; i < VMA_MAX; i++) {
                if (!procs[current_idx].vmas[i].used) { vma_idx = i; break; }
            }
            if (vma_idx < 0) return (uint64_t)MAP_FAILED;

            if (flags & 0x01) { /* MAP_SHARED == 0x01 from user */
                if (addr == 0) {
                    uint64_t hint = 0x40000000;
                    for (int i = 0; i < VMA_MAX; i++) {
                        if (procs[current_idx].vmas[i].used &&
                            procs[current_idx].vmas[i].end > hint)
                            hint = procs[current_idx].vmas[i].end;
                    }
                    addr = (hint + 0xFFF) & ~0xFFF;
                }
                procs[current_idx].vmas[vma_idx].start  = addr;
                procs[current_idx].vmas[vma_idx].end    = addr + length;
                procs[current_idx].vmas[vma_idx].prot   = prot;
                procs[current_idx].vmas[vma_idx].flags  = flags;
                procs[current_idx].vmas[vma_idx].fd     = fd;
                procs[current_idx].vmas[vma_idx].foff   = (uint64_t)offset;
                procs[current_idx].vmas[vma_idx].used   = 1;
                procs[current_idx].vma_count++;
                return addr;
            }

            if (addr == 0) {
                uint64_t hint = 0x40000000;
                for (int i = 0; i < VMA_MAX; i++) {
                    if (procs[current_idx].vmas[i].used &&
                        procs[current_idx].vmas[i].end > hint)
                        hint = procs[current_idx].vmas[i].end;
                }
                addr = (hint + 0xFFF) & ~0xFFF;
            }

            for (uint64_t v = addr; v < addr + length; v += 0x1000) {
                uint64_t phys = page_alloc();
                if (!phys) return (uint64_t)MAP_FAILED;
                memset((void *)(uintptr_t)phys, 0, 4096);
                uint64_t vm_flags = PG_PRESENT | PG_USER;
                if (prot & 2) vm_flags |= PG_WRITE;
                if (!(prot & 4)) vm_flags |= PG_NX;
                vmm_map(v, phys, vm_flags);
            }

            procs[current_idx].vmas[vma_idx].start = addr;
            procs[current_idx].vmas[vma_idx].end   = addr + length;
            procs[current_idx].vmas[vma_idx].prot  = prot;
            procs[current_idx].vmas[vma_idx].flags = flags;
            procs[current_idx].vmas[vma_idx].fd    = -1;
            procs[current_idx].vmas[vma_idx].foff  = 0;
            procs[current_idx].vmas[vma_idx].used  = 1;
            procs[current_idx].vma_count++;
            return addr;
        }
    case SC_MUNMAP:
        {
            uint64_t addr   = arg1;
            uint64_t length = arg2;
            if (length == 0) return 0;
            length = (length + 0xFFF) & ~0xFFF;

            for (int i = 0; i < VMA_MAX; i++) {
                struct vma *v = &procs[current_idx].vmas[i];
                if (!v->used) continue;
                if (addr >= v->start && addr < v->end) {
                    if (v->fd >= 0 && (v->flags & 0x01)) {
                        /* MAP_SHARED: write dirty pages back */
                        for (uint64_t p = addr; p < addr + length && p < v->end; p += 0x1000) {
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
                    for (uint64_t p = addr; p < addr + length && p < v->end; p += 0x1000)
                        vmm_unmap(p);
                    v->used = 0;
                    procs[current_idx].vma_count--;
                    break;
                }
            }
            return 0;
        }
    case SC_MSYNC:
        {
            uint64_t addr   = arg1;
            uint64_t length = arg2;
            if (length == 0) return 0;

            for (int i = 0; i < VMA_MAX; i++) {
                struct vma *v = &procs[current_idx].vmas[i];
                if (!v->used) continue;
                if (addr >= v->start && addr < v->end) {
                    if (v->fd >= 0 && (v->flags & 0x01)) {
                        for (uint64_t p = addr; p < addr + length && p < v->end; p += 0x1000) {
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
                    break;
                }
            }
            return 0;
        }
    }
    return 0;
}
