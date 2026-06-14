.intel_syntax noprefix
.code64

.extern syscall_handler

.section .data
.align 8
.global syscall_kernel_rsp
syscall_kernel_rsp: .quad 0
user_rsp_save: .quad 0

.section .text
.global syscall_entry
syscall_entry:
    mov [user_rsp_save], rsp
    mov rsp, [syscall_kernel_rsp]

    push rcx
    push r11
    push [user_rsp_save]

    mov r8, rdi
    mov rdi, rax
    mov rax, r8
    mov r8, rdx
    mov rdx, rsi
    mov rsi, rax
    mov rcx, r8
    call syscall_handler

    pop rax
    pop r11
    pop rcx
    mov rsp, rax

    sysretq
