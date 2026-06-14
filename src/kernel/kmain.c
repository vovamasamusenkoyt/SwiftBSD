#include "kernel.h"
#include "sched.h"
#include "pmm.h"
#include "vmm.h"
#include "ahci.h"

void idt_init(void);
void pit_init(int hz);
void keyboard_init(void);
void sched_init(void);
int  task_create(void (*func)(void));
void tss_init(void);
void module_subsystem_init(void);
void module_load_all(const struct kernel_api *api);
void kheap_init(void);
void user_proc_init(void);
int  ahci_init(void);
int  pci_init(void);

extern uint64_t page_pml4[];
extern uint8_t _bss_end[];

static void task_a(void) {
    for (;;) {
        __asm__("pause");
    }
}

static void task_b(void) {
    for (;;) {
        __asm__("pause");
    }
}

void kmain(uint32_t mboot_info) {
    serial_init();
    idt_init();

    serial_puts("\n========================================\n");
    serial_puts("  SwiftBSD v0.3 - M3+M4\n");
    serial_puts("========================================\n");

    pmm_init(mboot_info);
    uint64_t mem = pmm_total_mem();
    serial_printf("[mem] %d MB (%d pages free)\n",
                  (unsigned int)(mem / (1024 * 1024)),
                  (unsigned int)pmm_free_count());

    kheap_init();
    serial_puts("[kheap] heap initialized\n");

    pit_init(100);
    keyboard_init();
    sched_init();

    serial_printf("[mm] page_pml4 at %x\n", (unsigned)(uintptr_t)&page_pml4);

    vmm_init();

    tss_init();

    serial_puts("[tss] task register loaded\n");

    struct kernel_api api;
    api.kmalloc = kmalloc;
    api.kfree = kfree;
    api.printf = serial_printf;

    module_subsystem_init();
    module_load_all(&api);

    pci_init();
    ahci_init();

    uint8_t mbr[512];
    for (int p = 0; p < ahci_port_count(); p++) {
        int ret = ahci_read(p, 0, mbr, 1);
        if (ret > 0) {
            serial_printf("[ahci] port %d: sector 0 read OK (%d bytes)\n", p, ret);
            serial_puts("[ahci] first 64 bytes:");
            for (int i = 0; i < 64; i++) {
                if ((i % 16) == 0) serial_putc('\n');
                serial_printf(" %x", (unsigned)mbr[i]);
            }
            serial_putc('\n');
            break;
        } else {
            serial_printf("[ahci] port %d: read failed\n", p);
        }
    }

    serial_puts("[sched] creating tasks...\n");
    task_create(task_a);
    task_create(task_b);

    serial_puts("[user] launching user process\n");
    user_proc_init();

    serial_puts("[kmain] entering idle loop\n");

    __asm__("sti");

    for (;;)
        __asm__("hlt");
}
