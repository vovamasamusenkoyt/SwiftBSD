#include "elf.h"
#include "kernel.h"
#include "string.h"
#include <arch/x86_64/vmm.h>

extern uint64_t page_pml4[];

static uint64_t page_alloc_zero(void) {
    extern uint64_t page_alloc(void);
    uint64_t p = page_alloc();
    if (p) memset((void *)(uintptr_t)p, 0, 4096);
    return p;
}

int elf64_load(const void *data, uint64_t *entry_out) {
    const elf64_hdr_t *hdr = (const elf64_hdr_t *)data;

    if (*(const uint32_t *)hdr->e_ident != ELF_MAGIC) return -1;
    if (hdr->e_ident[ELF64_EI_CLASS] != ELFCLASS64) return -1;
    if (hdr->e_type != ET_EXEC) return -1;
    if (hdr->e_machine != EM_X86_64) return -1;

    const elf64_phdr_t *phdr = (const elf64_phdr_t *)((uint8_t *)data + hdr->e_phoff);

    for (int i = 0; i < hdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;

        uint64_t vaddr = phdr[i].p_vaddr;
        uint64_t filesz = phdr[i].p_filesz;
        uint64_t memsz = phdr[i].p_memsz;
        uint64_t offset = phdr[i].p_offset;
        uint32_t pflags = phdr[i].p_flags;

        uint64_t end = vaddr + memsz;
        for (uint64_t page = vaddr & ~0xFFF; page < end; page += 0x1000) {
            uint64_t phys = page_alloc_zero();
            if (!phys) return -1;

            int vm_flags = PG_PRESENT | PG_USER;
            if (pflags & 2) vm_flags |= PG_WRITE;
            if (!(pflags & 1)) vm_flags |= PG_NX;

            vmm_map(page, phys, vm_flags);
        }

        uint64_t copy = filesz < memsz ? filesz : memsz;
        uint8_t *dst = (uint8_t *)vaddr;
        const uint8_t *src = (const uint8_t *)data + offset;
        for (uint64_t j = 0; j < copy; j++)
            dst[j] = src[j];
    }

    {
        uint64_t stack_phys = page_alloc_zero();
        if (!stack_phys) return -1;
        vmm_map(0x7F000000, stack_phys,
                PG_PRESENT | PG_WRITE | PG_USER | PG_NX);
        stack_phys = page_alloc_zero();
        if (!stack_phys) return -1;
        vmm_map(0x7F001000, stack_phys,
                PG_PRESENT | PG_WRITE | PG_USER | PG_NX);
    }

    *entry_out = hdr->e_entry;
    return 0;
}