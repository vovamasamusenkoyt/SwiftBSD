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

/* Userland ELF binaries embedded from build/*.elf */
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
    int fd = swiftfs2_open(path, O_RDONLY);
    if (fd >= 0) { swiftfs2_close(fd); return; }

    uint64_t sz = (uint64_t)end - (uint64_t)start;
    fd = swiftfs2_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    swiftfs2_write(fd, start, sz);
    swiftfs2_close(fd);
}

void kmain(uint32_t mboot_info) {
    serial_init();
    idt_init();

    serial_puts("\n=== SwiftBSD v0.3 ===\n");

    pmm_init(mboot_info);
    uint64_t mem = pmm_total_mem();
    serial_printf("[mem] %d MB\n",
                  (unsigned)(mem / (1024 * 1024)));

    kheap_init();

    pit_init(100);
    keyboard_init();
    sched_init();

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

    uint8_t buf[512];
    for (int p = 0; p < ahci_port_count(); p++) {
        int ret = ahci_read(p, 0, buf, 1);
        if (ret > 0) {
            serial_printf("[ahci] port %d: sector 0 read OK\n", p);
            break;
        }
    }

    memset(buf, 0xAA, 512);
    ahci_write(0, 1024, buf, 1);

    /* Mount SwiftFS v2 and prepare userland */
    if (swiftfs2_mount(0) == 0) {
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

        /* Read shell and launch it via ELF loader */
        int fd = swiftfs2_open("/bin/shell", O_RDONLY);
        if (fd >= 0) {
            uint32_t cap = 65536, size = 0;
            uint8_t *data = kmalloc(cap);
            uint8_t tmp[512];
            int n;
            while ((n = swiftfs2_read(fd, tmp, sizeof(tmp))) > 0) {
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
            swiftfs2_close(fd);

            if (size > 0) {
                uint64_t entry;
                if (elf64_load(data, &entry) == 0) {
                    serial_puts("[kmain] launching shell\n");
                    uint64_t kernel_rsp = (uint64_t)stack_top;
                    tss_set_kernel_stack(kernel_rsp);
                    syscall_kernel_rsp = kernel_rsp;
                    user_entry(entry, 0x7F003000);
                } else {
                    serial_printf("[kmain] elf64_load failed for shell\n");
                }
            }
            kfree(data);
        }
    }

    extern void user_proc_init(void);
    user_proc_init();
    __asm__("sti");
    for (;;) __asm__("hlt");
}