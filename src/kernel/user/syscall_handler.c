#include "kernel.h"
#include "sched.h"

void syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    (void)arg2;
    (void)arg3;
    switch (num) {
    case 0:
        serial_puts((const char *)arg1);
        break;
    case 1:
        break;
    case 2:
        sched_yield();
        break;
    }
}
