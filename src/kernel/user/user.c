#include "kernel.h"
#include "pmm.h"
#include "vmm.h"
#include "tss.h"

extern uint64_t page_pml4[];
extern uint8_t _binary_build_user_prog_bin_start[];
extern uint8_t _binary_build_user_prog_bin_end[];
extern uint64_t syscall_kernel_rsp;
extern uint64_t stack_top[];
extern void user_entry(uint64_t entry, uint64_t rsp);

#define USER_BASE  0x8000000
#define USER_STACK 0x7F000000

void user_proc_init(void) {
    uint64_t code_size = (uint64_t)_binary_build_user_prog_bin_end
                       - (uint64_t)_binary_build_user_prog_bin_start;
    uint64_t code_pages = (code_size + 4095) / 4096;

    serial_printf("[user] binary at %x size %d\n",
                  (unsigned)(uintptr_t)_binary_build_user_prog_bin_start,
                  (unsigned)code_size);

    for (uint64_t i = 0; i < code_pages; i++) {
        uint64_t phys = page_alloc();
        if (!phys) {
            serial_puts("[user] OOM for code\n");
            return;
        }

        for (int j = 0; j < 4096; j++)
            ((uint8_t *)phys)[j] = 0;

        uint8_t *src = _binary_build_user_prog_bin_start + i * 4096;
        uint64_t copy = code_size - i * 4096;
        if (copy > 4096) copy = 4096;
        for (uint64_t j = 0; j < copy; j++)
            ((uint8_t *)phys)[j] = src[j];

        vmm_map(USER_BASE + i * 4096, phys,
                PG_PRESENT | PG_USER);
    }

    uint64_t stack_phys = page_alloc();
    if (!stack_phys) {
        serial_puts("[user] OOM for stack\n");
        return;
    }
    for (int j = 0; j < 4096; j++)
        ((uint8_t *)stack_phys)[j] = 0;

    vmm_map(USER_STACK, stack_phys,
            PG_PRESENT | PG_WRITE | PG_USER | PG_NX);

    serial_printf("[user] code at %x, stack at %x\n",
                  (unsigned)USER_BASE, (unsigned)USER_STACK);

    uint64_t kernel_rsp = (uint64_t)stack_top;

    tss_set_kernel_stack(kernel_rsp);
    syscall_kernel_rsp = kernel_rsp;

    serial_printf("[user] kernel RSP=%x\n", (unsigned)kernel_rsp);

    user_entry(USER_BASE, USER_STACK + 4096);
}
