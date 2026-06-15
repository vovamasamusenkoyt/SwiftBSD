OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

KERNEL_PHYS = 0x200000;

SECTIONS
{
    . = KERNEL_PHYS;

    .multiboot2 : ALIGN(8) {
        *(.multiboot2)
    }

    .text32 : ALIGN(4096) {
        *(.text32)
    }

    .text64 : ALIGN(4096) {
        _text64_start = .;
        *(.text64)
        *(.text)
        _text64_end = .;
    }

    .rodata : ALIGN(4096) {
        *(.rodata)
        *(.rodata.*)
    }

    .data : ALIGN(4096) {
        *(.data)
        *(.data.*)
    }

    .bss : ALIGN(4096) {
        _bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
        *(.lbss)
        _bss_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
        *(.debug*)
    }
}
