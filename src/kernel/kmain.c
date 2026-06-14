#include "kernel.h"
#include "sched.h"
#include "pmm.h"
#include "vmm.h"
#include "ahci.h"
#include "string.h"
#include "swiftfs2.h"

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
int  user_proc_load(const void *data, uint32_t size);
int  ahci_init(void);
int  pci_init(void);

extern uint64_t page_pml4[];
extern uint8_t _bss_end[];
extern uint8_t _binary_build_user_prog_bin_start[];
extern uint8_t _binary_build_user_prog_bin_end[];

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

    uint8_t buf[512];
    for (int p = 0; p < ahci_port_count(); p++) {
        int ret = ahci_read(p, 0, buf, 1);
        if (ret > 0) {
            serial_printf("[ahci] port %d: sector 0 read OK (%d bytes)\n", p, ret);
            serial_printf("[ahci] sector 0 signature=%x %x\n",
                buf[0x1FE], buf[0x1FF]);
            break;
        } else {
            serial_printf("[ahci] port %d: read failed\n", p);
        }
    }

    /* Write test: write/read-back high sector (avoid FS at block 0-2) */
    memset(buf, 0xAA, 512);
    int wret = ahci_write(0, 1024, buf, 1);
    serial_printf("[ahci] write sector 1024: %d bytes\n", wret);
    memset(buf, 0, 512);
    int rret = ahci_read(0, 1024, buf, 1);
    serial_printf("[ahci] read back sector 1024: %d bytes (first=%x)\n", rret, buf[0]);

    /* Mount SwiftFS v2 */
    int user_loaded = 0;
    if (swiftfs2_mount(0) == 0) {
        int fd = swiftfs2_open("/user.bin", O_RDONLY);
        if (fd >= 0) {
            /* Read file completely */
            uint8_t *user_code = 0;
            uint32_t sz = 0, cap = 4096;
            user_code = kmalloc(cap);
            int n;
            while ((n = swiftfs2_read(fd, user_code + sz, cap - sz)) > 0) {
                sz += n;
                if (cap - sz < 256) {
                    cap *= 2;
                    uint8_t *newp = kmalloc(cap);
                    memcpy(newp, user_code, sz);
                    kfree(user_code);
                    user_code = newp;
                }
            }
            swiftfs2_close(fd);
            if (sz > 0) {
                serial_printf("[swiftfs2] loaded /user.bin (%d bytes)\n", sz);
                user_proc_load(user_code, sz);
                kfree(user_code);
                user_loaded = 1;
            }
        } else {
            uint64_t code_size = (uint64_t)_binary_build_user_prog_bin_end
                               - (uint64_t)_binary_build_user_prog_bin_start;
            serial_printf("[swiftfs2] creating /user.bin (%u bytes)\n", (unsigned)code_size);
            int wfd = swiftfs2_open("/user.bin", O_WRONLY | O_CREAT | O_TRUNC);
            if (wfd >= 0) {
                int wr = swiftfs2_write(wfd, _binary_build_user_prog_bin_start, code_size);
                swiftfs2_close(wfd);
                serial_printf("[swiftfs2] wrote %d bytes\n", wr);
                swiftfs2_sync();

                fd = swiftfs2_open("/user.bin", O_RDONLY);
                if (fd >= 0) {
                    uint8_t *uc = kmalloc(code_size);
                    int n = swiftfs2_read(fd, uc, code_size);
                    if (n == (int)code_size) {
                        serial_printf("[swiftfs2] loaded /user.bin (%d bytes)\n", n);
                        user_proc_load(uc, code_size);
                        user_loaded = 1;
                    }
                    kfree(uc);
                    swiftfs2_close(fd);
                }
            }
        }
    }

    if (!user_loaded) {
        serial_puts("[user] FS load failed, using embedded binary\n");
        user_proc_init();
    }

    serial_puts("[sched] creating tasks...\n");
    task_create(task_a);
    task_create(task_b);

    serial_puts("[kmain] entering idle loop\n");

    __asm__("sti");

    for (;;)
        __asm__("hlt");
}
