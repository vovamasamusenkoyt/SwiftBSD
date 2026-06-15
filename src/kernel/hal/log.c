#include "kernel.h"

#define ANSI_RESET  "\033[0m"
#define ANSI_GREEN  "\033[32m"
#define ANSI_RED    "\033[31m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_BOLD   "\033[1m"

extern uint64_t timer_get_ticks(void);

static void pad_uint32(uint32_t n, int digits) {
    char buf[12];
    int pos = 11;
    buf[11] = 0;
    for (int i = 0; i < digits; i++) {
        buf[--pos] = '0' + (n % 10);
        n /= 10;
    }
    serial_puts(&buf[pos]);
}

static void log_timestamp(void) {
    uint64_t jiffies = timer_get_ticks();
    uint32_t sec = (uint32_t)(jiffies / 100);
    uint32_t us = (uint32_t)(jiffies % 100) * 10000;
    serial_puts("[  ");
    serial_printf("%u", sec);
    serial_putc('.');
    pad_uint32(us, 6);
    serial_puts("] ");
}

void log_info(const char *fmt, ...) {
    log_timestamp();
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    serial_vprintf(fmt, args);
    __builtin_va_end(args);
    serial_putc('\n');
}

void log_ok(const char *fmt, ...) {
    log_timestamp();
    serial_puts(" " ANSI_GREEN "[  OK  ]" ANSI_RESET " ");
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    serial_vprintf(fmt, args);
    __builtin_va_end(args);
    serial_putc('\n');
}

void log_fail(const char *fmt, ...) {
    log_timestamp();
    serial_puts(" " ANSI_RED "[ FAIL ]" ANSI_RESET " ");
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    serial_vprintf(fmt, args);
    __builtin_va_end(args);
    serial_putc('\n');
}

void log_warn(const char *fmt, ...) {
    log_timestamp();
    serial_puts(" " ANSI_YELLOW "[ WARN ]" ANSI_RESET " ");
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    serial_vprintf(fmt, args);
    __builtin_va_end(args);
    serial_putc('\n');
}

void log_raw(const char *fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    serial_vprintf(fmt, args);
    __builtin_va_end(args);
}
