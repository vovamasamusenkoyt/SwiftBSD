#include "idt.h"
#include "pic.h"
#include "kernel.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"

void keyboard_irq(void);
void timer_irq(void);
extern void syscall_entry(void);

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idtr idtr;
extern void *isr_stub_table[48];

static const char *const exc_names[] = {
    "DE", "DB", "NMI", "BP",  "OF", "BR", "UD", "NM",
    "DF", "CSO","TS",  "NP",  "SS", "GP", "PF", "RSV",
    "MF", "AC", "MC",  "XM",  "VE", "CP", "RSV","RSV",
    "RSV","RSV","RSV", "RSV", "RSV","RSV","SX", "RSV",
};

static void idt_set(int i, void *handler, uint8_t type) {
    uint64_t addr = (uint64_t)handler;
    idt[i].offset_low  = addr & 0xFFFF;
    idt[i].selector    = 0x08;
    idt[i].ist         = 0;
    idt[i].type        = type;
    idt[i].offset_mid  = (addr >> 16) & 0xFFFF;
    idt[i].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[i].reserved    = 0;
}

static void syscall_init(void) {
    uint64_t star = (0x08ULL << 32) | (0x10ULL << 48);
    uint64_t lstar = (uint64_t)syscall_entry;
    uint64_t sfmask = 0x200;

    __asm__ volatile("wrmsr" : : "c"(0xC0000081), "a"((uint32_t)star), "d"((uint32_t)(star >> 32)));
    __asm__ volatile("wrmsr" : : "c"(0xC0000082), "a"((uint32_t)lstar), "d"((uint32_t)(lstar >> 32)));
    __asm__ volatile("wrmsr" : : "c"(0xC0000084), "a"((uint32_t)sfmask), "d"(0));
}

void idt_init(void) {
    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt;

    for (int i = 0; i < 48; i++)
        idt_set(i, isr_stub_table[i], (i < 32) ? 0x8F : 0x8E);

    pic_remap();
    pic_set_mask(~((1 << 0) | (1 << 1)));

    __asm__ volatile("lidtq %0" : : "m"(idtr));

    syscall_init();
}

void exception_handler(struct exception_frame *frame) {
    if (frame->vector == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

        int pf_present  = frame->error_code & 1;
        int pf_write    = frame->error_code & 2;
        int pf_user     = frame->error_code & 4;

        if (pf_present && pf_write && cr2 >= 0x1000000) {
            uint64_t page     = cr2 & ~0xFFF;
            uint64_t cur_cr3  = vmm_read_cr3();
            uint64_t new_phys = page_alloc();
            if (!new_phys) {
                serial_printf("[pf] OOM at %x\n", cr2);
                for (;;) __asm__("hlt");
            }
            __asm__ volatile("cli");
            uint64_t bounce[512];
            for (int i = 0; i < 512; i++)
                bounce[i] = ((volatile uint64_t *)page)[i];
            vmm_map_in_cr3(cur_cr3, page, new_phys,
                           PG_PRESENT | PG_WRITE | PG_USER | PG_NX);
            for (int i = 0; i < 512; i++)
                ((volatile uint64_t *)page)[i] = bounce[i];
            __asm__ volatile("sti");
            return;
        }

        if (!pf_present && pf_write && cr2 >= 0x1000000) {
            uint64_t page = cr2 & ~0xFFF;
            uint64_t phys = vmm_alloc_page();
            if (!phys) {
                serial_printf("[pf] OOM at %x\n", cr2);
                for (;;) __asm__("hlt");
            }
            vmm_map(page, phys, PG_PRESENT | PG_WRITE | PG_NX | (pf_user ? PG_USER : 0));
            return;
        }

        /* Demand paging: file-backed or anonymous VMA (not present) */
        if (!pf_present && cr2 >= 0x1000000) {
            uint64_t page = cr2 & ~0xFFF;
            struct process *p = &procs[current_idx];
            for (int i = 0; i < VMA_MAX; i++) {
                struct vma *v = &p->vmas[i];
                if (!v->used || cr2 < v->start || cr2 >= v->end) continue;

                uint64_t phys = vmm_alloc_page();
                if (!phys) {
                    serial_printf("[pf] OOM at %x\n", cr2);
                    for (;;) __asm__("hlt");
                }
                uint64_t pg_flags = PG_PRESENT | PG_USER;
                if (pf_write && (v->prot & 2)) pg_flags |= PG_WRITE;
                if (!(v->prot & 4)) pg_flags |= PG_NX;
                vmm_map(page, phys, pg_flags);

                if (v->fd >= 0) {
                    uint64_t foff = v->foff + (page - v->start);
                    vfs_lseek(v->fd, (int64_t)foff, SEEK_SET);
                    vfs_read(v->fd, (void *)(uintptr_t)page, 4096);
                } else {
                    /* Anonymous: page already zeroed by vmm_alloc_page */
                }
                return;
            }
        }

        serial_printf("\n!!! PF (%x), ERR %x\n", frame->vector, frame->error_code);
        serial_printf("CR2: %x  RIP: %x  RSP: %x\n", cr2, frame->rip, frame->rsp);
        serial_printf("RAX: %x  RBX: %x  RCX: %x  RDX: %x\n",
                      frame->rax, frame->rbx, frame->rcx, frame->rdx);
        serial_printf("RSI: %x  RDI: %x  RBP: %x\n",
                      frame->rsi, frame->rdi, frame->rbp);
        serial_printf("R8:  %x  R9:  %x  R10: %x  R11: %x\n",
                      frame->r8, frame->r9, frame->r10, frame->r11);
        serial_printf("R12: %x  R13: %x  R14: %x  R15: %x\n",
                      frame->r12, frame->r13, frame->r14, frame->r15);
        for (;;) __asm__("hlt");
    }

    const char *name = (frame->vector < 32) ? exc_names[frame->vector] : "INT";
    serial_printf("\n!!! %s (%x), ERR %x\n",
                  name, frame->vector, frame->error_code);
    serial_printf("RIP: %x  RSP: %x  RFLAGS: %x\n",
                  frame->rip, frame->rsp, frame->rflags);
    serial_printf("RAX: %x  RBX: %x  RCX: %x  RDX: %x\n",
                  frame->rax, frame->rbx, frame->rcx, frame->rdx);
    serial_printf("RSI: %x  RDI: %x  RBP: %x\n",
                  frame->rsi, frame->rdi, frame->rbp);
    serial_printf("R8:  %x  R9:  %x  R10: %x  R11: %x\n",
                  frame->r8, frame->r9, frame->r10, frame->r11);
    serial_printf("R12: %x  R13: %x  R14: %x  R15: %x\n",
                  frame->r12, frame->r13, frame->r14, frame->r15);

    for (;;) __asm__("hlt");
}

void irq_handler(struct exception_frame *frame) {
    int irq = frame->vector - 32;

    if (irq == 0)
        timer_irq();
    else if (irq == 1)
        keyboard_irq();

    pic_send_eoi(irq);
}
