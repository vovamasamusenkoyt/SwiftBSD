#include "kernel.h"
#include "sched.h"
#include "swiftfs2.h"
#include "elf.h"
#include "string.h"

#define SC_PUTS  0
#define SC_NOP   1
#define SC_YIELD 2
#define SC_OPEN  3
#define SC_READ  4
#define SC_WRITE 5
#define SC_CLOSE 6
#define SC_EXEC  7
#define SC_EXIT  8

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
                /* stdin: read from serial port */
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
                /* stdout: write to serial port */
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
            if (fd <= 1) return 0; /* stdin/stdout: no-op */
            return swiftfs2_close(fd);
        }
    case SC_EXEC:
        {
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

            uint64_t entry;
            if (elf64_load(file_data, &entry) < 0) {
                kfree(file_data);
                return -1;
            }
            kfree(file_data);

            extern void user_entry(uint64_t entry, uint64_t rsp);
            extern uint64_t syscall_kernel_rsp;
            extern uint64_t stack_top[];
            extern void tss_set_kernel_stack(uint64_t rsp0);
            uint64_t krsp = (uint64_t)stack_top;
            tss_set_kernel_stack(krsp);
            syscall_kernel_rsp = krsp;
            user_entry(entry, 0x7F002000);
            return 0;
        }
    case SC_EXIT:
        serial_puts("[user] exit\n");
        for (;;) __asm__("hlt");
    }
    return 0;
}