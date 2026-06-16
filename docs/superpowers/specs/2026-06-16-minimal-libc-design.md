# Minimal libc for SwiftBSD

## Goal

Allow userland programs to be written as normal C programs
(`#include <stdio.h>`, `int main(int argc, char *argv[])`)
instead of bare `-nostdlib` binaries.

## Approach

Build `build/libc.a` + `build/crt0.o`.  Programs link:
```
gcc -ffreestanding -static -no-pie -I src/libc/include \
    -T src/userland/user.ld build/crt0.o prog.c -L build -lc -o prog.elf
```

## Files

```
src/libc/
  crt0.S               — _start → main(argc, argv); exit(ret)
  include/
    stdio.h              — FILE, stdin/out/err, printf, putchar, fopen...
    stdlib.h             — exit, malloc, free, atoi, strtol...
    string.h             — strlen, strcpy, strcmp, memcpy, memset...
    unistd.h             — open, read, write, close, brk, fork, exec...
    fcntl.h              — O_RDONLY, O_WRONLY...
    sys/stat.h           — stat, S_ISDIR, S_ISREG...
    ctype.h              — isdigit, isspace, isalpha, toupper...
    errno.h              — errno
    assert.h             — assert macro
  src/
    printf.c             — vfprintf (full format strings)
    stdio.c              — putchar, puts, fopen, fclose, fwrite...
    stdlib.c             — exit, malloc wrapper, atoi, strtol, rand...
    string.c             — memcpy, memset, memmove, strlen, strcmp...
    unistd.c             — syscall wrappers: open, read, write, close...
```

## Details

- **crt0.S**: zeros BSS (via `_bss_start`/`_bss_end` from linker), calls `main(argc, argv)`, exits with return value.
- **printf**: full vfprintf with `d,i,u,o,x,X,p,c,s,n,%`; flags `-+0# `; width/precision `.*`; length `l,ll,h,hh,z,t,j`. Uses callback `void (*out)(char c, void *arg)` so the same parser serves `fprintf/vfprintf/vprintf/sprintf/snprintf`.
- **FILE**: minimal struct `{int fd, int flags, int eof, int error}`. No buffering for v1. `stdin=fd0`, `stdout=fd1`, `stderr=fd1`.
- **malloc**: wraps the existing `src/userland/malloc.h`.
- **syscalls**: libc source files include `src/userland/syscall.h` for inline assembly wrappers.

## Transition

Userland programs (`shell.c`, `ls.c`, `cat.c`, `echo.c`) are updated:
- `void _start(void)` → `int main(int argc, char *argv[])`
- `#include "syscall.h"` → `#include <stdio.h>` / `#include <unistd.h>`
- `print(...)` → `printf(...)`

## Makefile

New rules:
```
build/libc_crt0.o: src/libc/crt0.S           (gcc -x assembler-with-cpp)
build/libc_%.o:    src/libc/src/%.c           (gcc, -I src/libc/include -I src/userland)
build/libc.a:      $(LIBC_OBJS)               (ar rcs)

build/%.o:         src/userland/%.c           (gcc -c, -I src/libc/include)
build/%.elf:       build/%.o build/libc_crt0.o build/libc.a  (ld -T user.ld, -lc)
```
