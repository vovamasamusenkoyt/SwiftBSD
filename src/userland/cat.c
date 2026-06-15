#include <stdint.h>
#include "syscall.h"

void _start(void) {
    const char *path = ARGS_PAGE;
    if (path[0] == 0) { print("usage: cat <file>\n"); exit(); }

    int fd = open(path, O_RDONLY);
    if (fd < 0) { print("cat: cannot open "); print(path); print("\n"); exit(); }

    stat_t st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.mode)) {
        print("cat: "); print(path); print(" is a directory\n");
        close(fd);
        exit();
    }

    char buf[256];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);
    close(fd);
    exit();
}
