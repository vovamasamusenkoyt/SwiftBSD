.intel_syntax noprefix
.code64

.extern syscall_handler

.section .data
.align 8
.global syscall_kernel_rsp
syscall_kernel_rsp: .quad 0
user_rsp_save: .quad 0
retval_save: .quad 0

.section .text
.global syscall_entry
syscall_entry:
    mov [user_rsp_save], rsp
    mov rsp, [syscall_kernel_rsp]

    push rdi
    push rsi
    push rdx
    push rcx
    push r8
    push r9
    push r10
    push r11

    mov rdi, rax
    mov rsi, [rsp+56]
    mov rdx, [rsp+48]
    mov rcx, [rsp+40]
    call syscall_handler

    mov [retval_save], rax

    pop r11
    pop r10
    pop r9
    pop r8
    pop rcx
    pop rdx
    pop rsi
    pop rdi

    mov rax, [retval_save]
    mov rsp, [user_rsp_save]
    sysretq
