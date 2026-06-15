#pragma once
#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE 4096
#define KHEAP_SIZE (1024 * 1024)

void kmain(uint32_t mboot_info);

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_printf(const char *fmt, ...);
void serial_vprintf(const char *fmt, __builtin_va_list args);
int  serial_haschar(void);
char serial_getc(void);

#include "log.h"

struct kernel_api {
    void *(*kmalloc)(size_t size);
    void (*kfree)(void *ptr);
    void (*printf)(const char *fmt, ...);
};

void *kmalloc(size_t size);
void kfree(void *ptr);
