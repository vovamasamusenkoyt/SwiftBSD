#include "sched.h"
#include "kernel.h"

struct irq_frame {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rdi, rsi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t err, vec;
    uint64_t rip, cs, rflags, rsp, ss;
};

static struct task tasks[MAX_TASKS];
static int current;
static int count;

void sched_init(void) {
    tasks[0].frame = 0;
    tasks[0].state = 1;
    current = 0;
    count = 1;
}

int task_create(void (*func)(void)) {
    if (count >= MAX_TASKS)
        return -1;

    void *base = (void *)(uintptr_t)(0x07F00000 + count * STACK_SIZE);
    struct irq_frame *f = (struct irq_frame *)((uint8_t *)base + STACK_SIZE - sizeof(*f));

    f->rax = 0; f->rbx = 0; f->rcx = 0; f->rdx = 0;
    f->rdi = 0; f->rsi = 0; f->rbp = 0;
    f->r8 = 0;  f->r9 = 0;  f->r10 = 0; f->r11 = 0;
    f->r12 = 0; f->r13 = 0; f->r14 = 0; f->r15 = 0;
    f->err = 0; f->vec = 0;
    f->rip    = (uint64_t)func;
    f->cs     = 0x08;
    f->rflags = 0x202;
    f->rsp    = (uint64_t)(f + 1);
    f->ss     = 0x10;

    tasks[count].frame = &f->rax;
    tasks[count].state = 1;
    count++;
    return count - 1;
}

void sched_yield(void) {
}

uint64_t *sched_irq_return(uint64_t *frame) {
    if (count < 2)
        return frame;

    tasks[current].frame = frame;

    int prev = current;
    do {
        current = (current + 1) % count;
    } while (tasks[current].state != 1);

    if (current == prev)
        return frame;

    return tasks[current].frame;
}
