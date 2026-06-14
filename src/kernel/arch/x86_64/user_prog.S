.intel_syntax noprefix
.code64
.section .user_prog, "ax"
.global _user_start
_user_start:
    mov rax, 0
    lea rdi, [rip + msg]
    mov rsi, 14
    syscall

    mov rax, 0
    lea rdi, [rip + msg2]
    mov rsi, 7
    syscall

1:  mov rax, 2
    syscall
    jmp 1b

.align 16
msg:  .ascii "Hello from R3!"
msg2: .ascii " loops\n"
