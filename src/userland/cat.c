#include <stdint.h>
#include "syscall.h"

void _start(void) {
    const char *path = "/usr/bin/shell";
    int fd = open(path, O_RDONLY);
    if (fd < 0) { print("cat: cannot open "); print(path); print("\n"); exit(); }

    char buf[256];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);
    close(fd);
    exit();
}
