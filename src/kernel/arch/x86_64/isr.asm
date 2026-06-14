.intel_syntax noprefix
.code64

.extern exception_handler
.extern sched_irq_return

.macro isr_noerr vec
.global isr\vec
isr\vec:
    push 0
    push \vec
    jmp exception_common
.endm

.macro isr_err vec
.global isr\vec
isr\vec:
    push \vec
    jmp exception_common
.endm

isr_noerr 0
isr_noerr 1
isr_noerr 2
isr_noerr 3
isr_noerr 4
isr_noerr 5
isr_noerr 6
isr_noerr 7
isr_err   8
isr_noerr 9
isr_err   10
isr_err   11
isr_err   12
isr_err   13
isr_err   14
isr_noerr 15
isr_noerr 16
isr_err   17
isr_noerr 18
isr_noerr 19
isr_noerr 20
isr_noerr 21
isr_noerr 22
isr_noerr 23
isr_noerr 24
isr_noerr 25
isr_noerr 26
isr_noerr 27
isr_noerr 28
isr_noerr 29
isr_err   30
isr_noerr 31

.macro isr_irq vec, irq
.global isr\vec
isr\vec:
    push 0
    push \vec
    jmp irq_common
.endm

isr_irq 32, 0
isr_irq 33, 1
isr_irq 34, 2
isr_irq 35, 3
isr_irq 36, 4
isr_irq 37, 5
isr_irq 38, 6
isr_irq 39, 7
isr_irq 40, 8
isr_irq 41, 9
isr_irq 42, 10
isr_irq 43, 11
isr_irq 44, 12
isr_irq 45, 13
isr_irq 46, 14
isr_irq 47, 15

exception_common:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rsi
    push rdi
    push rdx
    push rcx
    push rbx
    push rax

    mov rdi, rsp
    call exception_handler

    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rdi
    pop rsi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    add rsp, 16
    iretq

irq_common:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rsi
    push rdi
    push rdx
    push rcx
    push rbx
    push rax

    mov rdi, rsp
    call irq_handler

    mov rdi, rsp
    call sched_irq_return
    mov rsp, rax

    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rdi
    pop rsi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    add rsp, 16
    iretq

.section .rodata
.global isr_stub_table
isr_stub_table:
    .quad isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    .quad isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    .quad isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .quad isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    .quad isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39
    .quad isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47
