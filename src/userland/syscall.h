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
#define SC_HALT  10
#define SC_MEMINFO 11
#define SC_FORK   12
#define SC_WAIT   13
#define SC_GETPID 14
#define SC_BRK    15
#define SC_MMAP   16
#define SC_MUNMAP 17
#define SC_MSYNC  18
#define SC_PIPE   19
#define SC_DUP    20
#define SC_DUP2   21
#define SC_MPROTECT 22

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0x100
#define O_TRUNC     0x200

#define MAP_SHARED  1
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 4
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

#define ARGS_PAGE ((char *)0x7F004000)

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
static int exec(const char *p, const char *args) { return syscall(SC_EXEC, (long)p, (long)args, 0); }
static void _exit(int code) { syscall(SC_EXIT, code, 0, 0); }
static void exit(void) { _exit(0); }
static void halt(void) { syscall(SC_HALT, 0, 0, 0); }
static int fork(void) { return syscall(SC_FORK, 0, 0, 0); }
static int wait(int *status) { return syscall(SC_WAIT, (long)status, 0, 0); }
static int getpid(void) { return syscall(SC_GETPID, 0, 0, 0); }

/* Packed mmap args - flags|fd|offset in arg3 */
struct mmap_args { int flags; int fd; int pad; long long offset; };
static void *mmap(void *addr, unsigned long length, int prot, int flags, int fd, long long offset) {
    struct mmap_args a;
    a.flags  = flags;
    a.fd     = fd;
    a.pad    = 0;
    a.offset = offset;
    return (void *)syscall(SC_MMAP, (long)addr, length, *(long *)&a);
}
static int munmap(void *addr, unsigned long length) {
    return (int)syscall(SC_MUNMAP, (long)addr, length, 0);
}
static int msync(void *addr, unsigned long length) {
    return (int)syscall(SC_MSYNC, (long)addr, length, 0);
}
static int pipe(int fds[2]) { return (int)syscall(SC_PIPE, (long)fds, 0, 0); }
static int dup(int oldfd) { return (int)syscall(SC_DUP, oldfd, 0, 0); }
static int dup2(int oldfd, int newfd) { return (int)syscall(SC_DUP2, oldfd, newfd, 0); }
static int mprotect(void *addr, unsigned long length, int prot) {
    return (int)syscall(SC_MPROTECT, (long)addr, length, prot);
}

static void *sbrk(long increment) {
    long cur = syscall(SC_BRK, 0, 0, 0);
    if (increment == 0) return (void *)cur;
    long new = syscall(SC_BRK, cur + increment, 0, 0);
    return (new == -1) ? (void *)-1 : (void *)cur;
}
static long brk(void *addr) {
    return syscall(SC_BRK, (long)addr, 0, 0);
}

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