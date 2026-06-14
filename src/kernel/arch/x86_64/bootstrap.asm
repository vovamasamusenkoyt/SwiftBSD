.intel_syntax noprefix

.set KERNEL_VMA, 0

.section .multiboot2, "a"
.align 8
multiboot2_start:
    .long 0xE85250D6
    .long 0
    .long multiboot2_end - multiboot2_start
    .long 0x100000000 - (0xE85250D6 + 0 + (multiboot2_end - multiboot2_start))
    .word 0
    .word 0
    .long 8
multiboot2_end:

.section .text32, "ax"
.code32
.global _start
_start:
    lea esp, [stack_top - KERNEL_VMA]
    mov [mboot_magic - KERNEL_VMA], eax
    mov [mboot_info - KERNEL_VMA], ebx

    call check_cpuid
    call check_long_mode
    call setup_paging

    lea eax, [page_pml4 - KERNEL_VMA]
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5      # PAE
    mov cr4, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1           # SCE
    or eax, 1 << 8      # LME
    wrmsr

    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    lea eax, [gdt64 - KERNEL_VMA]
    mov [gdtr + 2 - KERNEL_VMA], eax
    lea eax, [gdtr - KERNEL_VMA]
    lgdt [eax]
    ljmp 0x08, long_mode_entry - KERNEL_VMA

check_cpuid:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    jne 1f
    lea esi, [msg_no_cpuid - KERNEL_VMA]
    call serial_write
2:  hlt
    jmp 2b
1:  ret

check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb 2f
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jnz 1f
2:  lea esi, [msg_no_long - KERNEL_VMA]
    call serial_write
3:  hlt
    jmp 3b
1:  ret

setup_paging:
    lea edi, [page_pml4 - KERNEL_VMA]
    lea eax, [page_pdpt - KERNEL_VMA + 7]
    mov [edi], eax

    lea edi, [page_pdpt - KERNEL_VMA]
    lea eax, [page_pd - KERNEL_VMA + 7]
    mov [edi], eax

    lea edi, [page_pd - KERNEL_VMA]
    mov eax, 0x83
    mov ecx, 64
1:  mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop 1b
    ret

serial_write:
    mov edx, 0x3F8
1:  lodsb
    test al, al
    jz 2f
    out dx, al
    jmp 1b
2:  ret

msg_no_cpuid: .asciz "ERR: CPUID not supported\n"
msg_no_long:  .asciz "ERR: Long mode not supported\n"

.align 16
gdt64:
    .quad 0                                # 0x00 null
    .quad 0x00AF9A0000000000               # 0x08 kernel code
    .quad 0x00AF920000000000               # 0x10 kernel data
    .quad 0x00AFF20000000000               # 0x18 user data (DPL=3)
    .quad 0x00AFFA0000000000               # 0x20 user code (DPL=3)
    .quad 0x0000000000000000               # 0x28 TSS low  (filled by C)
    .quad 0x0000000000000000               # 0x30 TSS high (filled by C)
gdtr:
    .word gdtr - gdt64 - 1
    .long 0

.section .data
.align 8
mboot_magic: .long 0
mboot_info:  .long 0

.section .bss
.align 4096
.global page_pml4
page_pml4:     .skip 4096
.global page_pdpt
page_pdpt:     .skip 4096
.global page_pd
page_pd:       .skip 4096
.align 16
stack_bottom: .skip 32768
.global stack_top
stack_top:

.code64
.section .text64, "ax"
long_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    lea rsp, [stack_top]
    xor rbp, rbp

    # Enable NX (Execute Disable) if supported
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb 1f
    mov eax, 0x80000001
    cpuid
    bt edx, 20
    jnc 1f
    mov ecx, 0xC0000080
    rdmsr
    or eax, 0x800
    wrmsr
    mov rax, cr4
    or rax, 0x800
    mov cr4, rax
1:

    mov rsi, offset dbg_msg
    mov dx, 0x3F8
1:  lodsb
    test al, al
    jz 2f
    out dx, al
    jmp 1b
2:

    mov edi, [mboot_info]
    call kmain

dbg_msg: .asciz "[64] Entered long mode\n"

1:  cli
    hlt
    jmp 1b
