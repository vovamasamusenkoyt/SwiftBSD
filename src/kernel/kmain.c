#include "kernel.h"
#include "sched.h"
#include "pmm.h"
#include "vmm.h"
#include "ahci.h"
#include "string.h"
#include "swiftfs2.h"
#include "elf.h"
#include "tss.h"
 
void idt_init(void);
void pit_init(int hz);
void keyboard_init(void);
void sched_init(void);
int  task_create(void (*func)(void));
void tss_init(void);
void module_subsystem_init(void);
void module_load_all(const struct kernel_api *api);
void kheap_init(void);
int  ahci_init(void);
int  pci_init(void);
 
extern uint64_t page_pml4[];
extern uint8_t _bss_end[];
extern uint64_t syscall_kernel_rsp;
extern uint64_t stack_top[];
extern void user_entry(uint64_t entry, uint64_t rsp);

extern uint64_t timer_get_ticks(void);

/* Userland ELF binaries embedded from build (binary glob) */
#define DEFINE_EMBEDDED(name) \
    extern uint8_t _binary_build_##name##_elf_start[]; \
    extern uint8_t _binary_build_##name##_elf_end[];

DEFINE_EMBEDDED(shell)
DEFINE_EMBEDDED(ls)
DEFINE_EMBEDDED(cat)
DEFINE_EMBEDDED(echo)

/* Write embedded ELF to FS if not present */
static void write_if_missing(const char *path,
                             uint8_t *start, uint8_t *end) {
    int fd = vfs_open(path, O_RDONLY);
    if (fd >= 0) { vfs_close(fd); return; }

    uint64_t sz = (uint64_t)end - (uint64_t)start;
    fd = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    vfs_write(fd, start, (uint32_t)sz);
    vfs_close(fd);
}

void vfs_init(void);
void swiftfs2_vfs_init(void);

void kmain(uint32_t mboot_info) {
    serial_init();
    idt_init();

    log_raw("SwiftBSD version 0.3\n");
    log_raw("Booting SwiftBSD kernel\n\n");

    pmm_init(mboot_info);
    uint64_t mem = pmm_total_mem();
    log_info("mem: %d MB available", (unsigned)(mem / (1024 * 1024)));

    log_raw("\n");
    kheap_init();

    log_raw("\n");
    pit_init(100);
    keyboard_init();
    sched_init();

    log_raw("\n");
    vmm_init();
    tss_init();

    log_raw("\n");
    struct kernel_api api;
    api.kmalloc = kmalloc;
    api.kfree = kfree;
    api.printf = serial_printf;

    module_subsystem_init();
    module_load_all(&api);

    log_raw("\n");
    pci_init();

    log_raw("\n");
    ahci_init();

    uint8_t buf[512];
    for (int p = 0; p < ahci_port_count(); p++) {
        int ret = ahci_read(p, 0, buf, 1);
        if (ret > 0) {
            log_info("ahci: port %d sector 0 read OK", p);
            break;
        }
    }

    memset(buf, 0xAA, 512);
    ahci_write(0, 1024, buf, 1);

    vfs_init();

    log_raw("\n");
    /* Mount SwiftFS v2 and prepare userland */
    if (swiftfs2_mount(0) == 0) {
        swiftfs2_vfs_init();
        /* Create /bin/ directory */
        swiftfs2_mkdir("/bin", 0755);

        write_if_missing("/bin/shell",
            _binary_build_shell_elf_start, _binary_build_shell_elf_end);
        write_if_missing("/bin/ls",
            _binary_build_ls_elf_start, _binary_build_ls_elf_end);
        write_if_missing("/bin/cat",
            _binary_build_cat_elf_start, _binary_build_cat_elf_end);
        write_if_missing("/bin/echo",
            _binary_build_echo_elf_start, _binary_build_echo_elf_end);
        swiftfs2_sync();

        /* Read shell and create init process (PID 1) */
        int fd = vfs_open("/bin/shell", O_RDONLY);
        if (fd >= 0) {
            uint32_t cap = 65536, size = 0;
            uint8_t *data = kmalloc(cap);
            uint8_t tmp[512];
            int n;
            while ((n = vfs_read(fd, tmp, sizeof(tmp))) > 0) {
                if (size + n > cap) {
                    cap *= 2;
                    uint8_t *np = kmalloc(cap);
                    memcpy(np, data, size);
                    kfree(data);
                    data = np;
                }
                memcpy(data + size, tmp, n);
                size += n;
            }
            vfs_close(fd);

            if (size > 0) {
                uint64_t entry;
                if (elf64_load(data, &entry) == 0) {
                    log_info("kernel: creating init process (PID 1)");

                    uint64_t init_kstack = (uint64_t)page_alloc();
                    if (init_kstack) {
                        int pid = proc_create(entry, 0x7F003000,
                                              vmm_read_cr3(), init_kstack);
                        if (pid < 0) {
                            log_fail("kernel: proc_create failed");
                        } else {
                            log_ok("kernel: init process created (PID %d)", pid);
                        }
                    }
                } else {
                    log_fail("kernel: elf64_load failed for shell");
                }
            }
            kfree(data);
        }
    }

    /* Register init in scheduler and launch directly via user_entry */
    current_idx = proc_index(1);
    if (current_idx < 0) {
        log_fail("kernel: init process not found");
        for (;;) __asm__("hlt");
    }
    sched_run();

    log_raw("\n");
    log_ok("Reached target Multi-User System\n");

    log_raw("Run /bin/shell as init process\n\n");
    extern void user_entry(uint64_t entry, uint64_t rsp);
    user_entry(0x8000000, 0x7F003000);

    for (;;) __asm__("hlt");
}
