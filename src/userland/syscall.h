#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#define SC_PUTS  0
#define SC_NOP   1
#define SC_YIELD 2
#define SC_OPEN  3
#define SC_READ  4
#define SC_WRITE 5
#define SC_CLOSE 6
#define SC_EXEC  7
#define SC_EXIT  8
#define SC_FSTAT 9

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0x100
#define O_TRUNC     0x200

static long syscall(long num, long a1, long a2, long a3) {
    long ret;
    asm volatile("syscall" : "=a"(ret)
                 : "a"(num), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return ret;
}

typedef struct {
    unsigned short mode;
    unsigned long long size;
} stat_t;

#define S_ISDIR(m)  ((m) & 0040000)

static void print(const char *s) { syscall(SC_PUTS, (long)s, 0, 0); }
static int open(const char *p, int f) { return syscall(SC_OPEN, (long)p, f, 0); }
static int read(int fd, void *b, unsigned long sz) { return syscall(SC_READ, fd, (long)b, sz); }
static int write(int fd, const void *b, unsigned long sz) { return syscall(SC_WRITE, fd, (long)b, sz); }
static int close(int fd) { return syscall(SC_CLOSE, fd, 0, 0); }
static int fstat(int fd, stat_t *st) { return syscall(SC_FSTAT, fd, (long)st, 0); }
static void exec(const char *p) { syscall(SC_EXEC, (long)p, 0, 0); }
static void exit(void) { syscall(SC_EXIT, 0, 0, 0); }

static int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

static int strlen(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n;
}

static int strncmp(const char *a, const char *b, unsigned long n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

#endif