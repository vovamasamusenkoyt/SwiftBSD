#include "process.h"
#include "kernel.h"
#include "pmm.h"
#include "vmm.h"
#include "tss.h"
#include "string.h"
#include "idt.h"

extern uint64_t syscall_kernel_rsp;
extern uint64_t syscall_frame_ptr;
extern uint64_t user_rsp_save;
extern uint64_t stack_top[];
extern uint64_t page_pml4[];

struct process procs[MAX_PROCS];
int current_idx;
static int next_pid;
static int nr_procs;

#define SYSCALL_STACK_SLOT_R11  0
#define SYSCALL_STACK_SLOT_R10  8
#define SYSCALL_STACK_SLOT_R9   16
#define SYSCALL_STACK_SLOT_R8   24
#define SYSCALL_STACK_SLOT_RCX  32
#define SYSCALL_STACK_SLOT_RDX  40
#define SYSCALL_STACK_SLOT_RSI  48
#define SYSCALL_STACK_SLOT_RDI  56

static int slot_alloc(void) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (!procs[i].used) return i;
    }
    return -1;
}

int pid_alloc(void) {
    int pid;
    for (int try = 0; try < 65536; try++) {
        pid = next_pid++;
        if (next_pid <= 0) next_pid = 1;
        int ok = 1;
        for (int i = 0; i < MAX_PROCS; i++) {
            if (procs[i].used && procs[i].pid == pid) { ok = 0; break; }
        }
        if (ok) return pid;
    }
    return -1;
}

void pid_free(int pid) {
    (void)pid;
}

int proc_index(int pid) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].used && procs[i].pid == pid) return i;
    }
    return -1;
}

int proc_create(uint64_t entry, uint64_t rsp, uint64_t cr3, uint64_t kstack) {
    int idx = slot_alloc();
    if (idx < 0) return -1;

    int pid = pid_alloc();
    if (pid < 0) return -1;

    for (uint64_t *p = (uint64_t *)(uintptr_t)kstack;
         p < (uint64_t *)(uintptr_t)(kstack + KSTACK_SIZE); p++)
        *p = 0;

    uint64_t *frame = (uint64_t *)(uintptr_t)(kstack + KSTACK_SIZE - 22 * 8);
    frame[0]  = 0;            /* rax */
    frame[1]  = 0;            /* rbx */
    frame[2]  = 0;            /* rcx */
    frame[3]  = 0;            /* rdx */
    frame[4]  = 0;            /* rdi */
    frame[5]  = 0;            /* rsi */
    frame[6]  = 0;            /* rbp */
    frame[7]  = 0;            /* r8 */
    frame[8]  = 0;            /* r9 */
    frame[9]  = 0;            /* r10 */
    frame[10] = 0;            /* r11 */
    frame[11] = 0;            /* r12 */
    frame[12] = 0;            /* r13 */
    frame[13] = 0;            /* r14 */
    frame[14] = 0;            /* r15 */
    frame[15] = 0;            /* vector */
    frame[16] = 0;            /* error_code */
    frame[17] = entry;        /* rip */
    frame[18] = 0x23;         /* cs */
    frame[19] = 0x202;        /* rflags */
    frame[20] = rsp;          /* user rsp */
    frame[21] = 0x1B;         /* ss */

    procs[idx].used      = 1;
    procs[idx].pid       = pid;
    procs[idx].state     = PROC_READY;
    procs[idx].exit_code = 0;
    procs[idx].parent    = 1;
    procs[idx].cr3        = cr3;
    procs[idx].kstack     = kstack;
    procs[idx].frame      = frame;
    procs[idx].heap_start = 0x10000000;
    procs[idx].heap_break = 0x10000000;
    for (int i = 0; i < VMA_MAX; i++)
        procs[idx].vmas[i].used = 0;
    procs[idx].vma_count  = 0;

    nr_procs++;
    return pid;
}

static void set_kernel_stack(uint64_t kstack) {
    uint64_t rsp_val = kstack + KSTACK_SIZE;
    syscall_kernel_rsp = rsp_val;
    tss_set_kernel_stack(rsp_val);
}

void sched_yield(void) {
    __asm__ volatile("sti; hlt");
}

void sched_init(void) {
    for (int i = 0; i < MAX_PROCS; i++)
        procs[i].used = 0;
    current_idx = 0;
    next_pid = 1;
    nr_procs = 0;
}

uint64_t *sched_irq_return(uint64_t *frame) {
    if (procs[current_idx].used)
        procs[current_idx].frame = frame;

    int prev = current_idx;
    int tries = 0;
    do {
        current_idx = (current_idx + 1) % MAX_PROCS;
        tries++;
    } while (tries <= MAX_PROCS &&
             (!procs[current_idx].used || procs[current_idx].state != PROC_READY));

    if (tries > MAX_PROCS || current_idx == prev)
        return frame;

    struct process *p = &procs[current_idx];
    set_kernel_stack(p->kstack);
    if (p->cr3) {
        __asm__ volatile("mov %0, %%cr3" : : "r"(p->cr3) : "memory");
    }
    return p->frame;
}

void sched_run(void) {
    struct process *p = &procs[current_idx];
    if (p->cr3) {
        __asm__ volatile("mov %0, %%cr3" : : "r"(p->cr3) : "memory");
    }
    set_kernel_stack(p->kstack);
}

static uint64_t build_irq_frame(uint64_t *sysframe, uint64_t rax_val,
                                uint64_t child_kstack)
{
    uint64_t *frame = (uint64_t *)(child_kstack + KSTACK_SIZE - 15 * 8 - 2 * 8);

    /* exception_frame layout: rax, rbx, rcx, rdx, rdi, rsi, rbp,
       r8, r9, r10, r11, r12, r13, r14, r15, vector, error_code,
       rip, cs, rflags, rsp, ss */
    frame[0]  = rax_val;                            /* rax */
    frame[1]  = 0;                                   /* rbx */
    frame[2]  = sysframe[SYSCALL_STACK_SLOT_RCX/8]; /* rcx = user rip */
    frame[3]  = sysframe[SYSCALL_STACK_SLOT_RDX/8]; /* rdx */
    frame[4]  = sysframe[SYSCALL_STACK_SLOT_RDI/8]; /* rdi */
    frame[5]  = sysframe[SYSCALL_STACK_SLOT_RSI/8]; /* rsi */
    frame[6]  = 0;                                   /* rbp */
    frame[7]  = sysframe[SYSCALL_STACK_SLOT_R8/8];  /* r8 */
    frame[8]  = sysframe[SYSCALL_STACK_SLOT_R9/8];  /* r9 */
    frame[9]  = sysframe[SYSCALL_STACK_SLOT_R10/8]; /* r10 */
    frame[10] = sysframe[SYSCALL_STACK_SLOT_R11/8]; /* r11 = user rflags */
    frame[11] = 0;                                   /* r12 */
    frame[12] = 0;                                   /* r13 */
    frame[13] = 0;                                   /* r14 */
    frame[14] = 0;                                   /* r15 */
    frame[15] = 0;                                   /* vector (dummy) */
    frame[16] = 0;                                   /* error_code (dummy) */
    frame[17] = sysframe[SYSCALL_STACK_SLOT_RCX/8]; /* rip = ret after syscall */
    frame[18] = 0x23;                                /* cs (user 64-bit) */
    frame[19] = 0x202;                               /* rflags (IF set) */
    frame[20] = user_rsp_save;                       /* rsp */
    frame[21] = 0x1B;                                /* ss (user data) */

    return (uint64_t)frame;
}

int proc_fork(void) {
    int parent_idx = current_idx;

    uint64_t parent_cr3 = procs[parent_idx].cr3;
    if (!parent_cr3) parent_cr3 = (uint64_t)(uintptr_t)page_pml4;

    uint64_t child_cr3 = vmm_clone_pml4(parent_cr3);
    if (!child_cr3) return -1;

    uint64_t kstack = (uint64_t)page_alloc();
    if (!kstack) return -1;

    uint64_t *sf = (uint64_t *)(uintptr_t)syscall_frame_ptr;
    uint64_t child_frame = build_irq_frame(sf, 0, kstack);

    int idx = slot_alloc();
    if (idx < 0) return -1;
    int pid = pid_alloc();
    if (pid < 0) return -1;

    procs[idx].used      = 1;
    procs[idx].pid       = pid;
    procs[idx].state     = PROC_READY;
    procs[idx].exit_code = 0;
    procs[idx].parent    = procs[parent_idx].pid;
    procs[idx].cr3        = child_cr3;
    procs[idx].kstack     = kstack;
    procs[idx].frame      = (uint64_t *)child_frame;
    procs[idx].heap_start = procs[parent_idx].heap_start;
    procs[idx].heap_break = procs[parent_idx].heap_break;
    for (int i = 0; i < VMA_MAX; i++)
        procs[idx].vmas[i] = procs[parent_idx].vmas[i];
    procs[idx].vma_count  = procs[parent_idx].vma_count;

    nr_procs++;
    return pid;
}

void proc_exit(int code) {
    int idx = current_idx;
    procs[idx].exit_code = code;
    procs[idx].state = PROC_ZOMBIE;
    serial_printf("[exit] PID %d exit(%d)\n", procs[idx].pid, code);
    __asm__ volatile("sti");
    for (;;) __asm__ volatile("hlt");
}

int proc_wait(int *code_out) {
    int my_pid = procs[current_idx].pid;

    for (;;) {
        for (int i = 0; i < MAX_PROCS; i++) {
            if (!procs[i].used) continue;
            if (procs[i].state != PROC_ZOMBIE) continue;
            if (procs[i].parent != my_pid) continue;

            int pid = procs[i].pid;
            if (code_out) *code_out = procs[i].exit_code;
            procs[i].used = 0;
            nr_procs--;
            return pid;
        }

        int has_child = 0;
        for (int i = 0; i < MAX_PROCS; i++) {
            if (!procs[i].used) continue;
            if (procs[i].parent == my_pid) { has_child = 1; break; }
        }
        if (!has_child) return -1;

        sched_yield();
    }
}
