#pragma once
#include <stdint.h>

struct exception_frame {
    uint64_t rax, rbx, rcx, rdx, rdi, rsi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

void idt_init(void);
void exception_handler(struct exception_frame *frame);
void irq_handler(struct exception_frame *frame);
