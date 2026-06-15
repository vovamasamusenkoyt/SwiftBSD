#include <stdint.h>
#include "syscall.h"

void _start(void) {
    const char *args = ARGS_PAGE;
    const char *path = args[0] ? args : "/";

    uint8_t raw[4096];
    int fd = open(path, O_RDONLY);
    if (fd < 0) { print("ls: cannot open "); print(path); print("\n"); exit(); }

    stat_t st;
    if (fstat(fd, &st) < 0 || !S_ISDIR(st.mode)) {
        print("ls: "); print(path); print(": not a directory\n");
        close(fd);
        exit();
    }

    int n = read(fd, raw, sizeof(raw));
    close(fd);
    if (n <= 0) exit();

    int pos = 0;
    while (pos < n) {
        uint32_t ino = *(uint32_t *)(raw + pos);
        if (ino == 0) break;
        uint16_t reclen = *(uint16_t *)(raw + pos + 4);
        uint8_t nlen = *(uint8_t *)(raw + pos + 6);
        uint8_t ftype = *(uint8_t *)(raw + pos + 7);
        if (reclen == 0 || nlen == 0) break;

        for (int i = 0; i < nlen; i++)
            write(1, raw + pos + 8 + i, 1);
        print(ftype == 2 ? "/\n" : "\n");
        pos += reclen;
    }
    exit();
}
