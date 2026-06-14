.intel_syntax noprefix
.code64

.global switch_to
switch_to:
    push r15
    push r14
    push r13
    push r12
    push rbx
    push rbp

    mov [rdi], rsp
    mov rsp, rsi

    pop rbp
    pop rbx
    pop r12
    pop r13
    pop r14
    pop r15
    ret
