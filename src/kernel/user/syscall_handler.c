#include "kernel.h"
#include "sched.h"
#include "swiftfs2.h"
#include "elf.h"
#include "string.h"
#include "pmm.h"

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

#define ARGS_PAGE ((char *)0x7F004000)

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
        return swiftfs2_open((const char *)arg1, (int)arg2);
    case SC_READ:
        {
            int fd = (int)arg1;
            if (fd == 0) {
                char *buf = (char *)arg2;
                uint32_t sz = (uint32_t)arg3;
                for (uint32_t i = 0; i < sz; i++)
                    buf[i] = serial_getc();
                return sz;
            }
            return swiftfs2_read(fd, (void *)arg2, (uint32_t)arg3);
        }
    case SC_WRITE:
        {
            int fd = (int)arg1;
            if (fd == 0 || fd == 1) {
                const char *buf = (const char *)arg2;
                uint32_t sz = (uint32_t)arg3;
                for (uint32_t i = 0; i < sz; i++)
                    serial_putc(buf[i]);
                return sz;
            }
            return swiftfs2_write(fd, (const void *)arg2, (uint32_t)arg3);
        }
    case SC_CLOSE:
        {
            int fd = (int)arg1;
            if (fd <= 1) return 0;
            return swiftfs2_close(fd);
        }
    case SC_EXEC:
        {
            /* Copy args from user space before ELF load overwrites memory */
            char args_buf[256];
            if (arg2) {
                const char *src = (const char *)arg2;
                int i;
                for (i = 0; i < 255 && src[i]; i++) args_buf[i] = src[i];
                args_buf[i] = 0;
            } else {
                args_buf[0] = 0;
            }

            int fd = swiftfs2_open((const char *)arg1, O_RDONLY);
            if (fd < 0) return -1;

            uint32_t size = 0, cap = 65536;
            uint8_t *file_data = kmalloc(cap);
            if (!file_data) { swiftfs2_close(fd); return -1; }

            uint8_t tmp[512];
            int n;
            while ((n = swiftfs2_read(fd, tmp, sizeof(tmp))) > 0) {
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
            swiftfs2_close(fd);
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
    case SC_FSTAT:
        return swiftfs2_fstat((int)arg1, (swiftfs2_stat_t *)arg2);
    case SC_EXIT:
        serial_printf("[user] PID %d exit\n", procs[current_idx].pid);
        proc_exit((int)arg1);
        __builtin_unreachable();
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
        return proc_fork();
    case SC_WAIT:
        {
            int code;
            int ret = proc_wait(&code);
            if (ret > 0 && arg1) {
                int *uptr = (int *)arg1;
                *uptr = code;
            }
            return ret;
        }
    case SC_GETPID:
        return procs[current_idx].pid;
    }
    return 0;
}
