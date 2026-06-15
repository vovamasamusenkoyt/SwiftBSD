#include <stdint.h>
#include "syscall.h"

void _start(void) {
    uint8_t raw[4096];
    int fd = open("/", O_RDONLY);
    if (fd < 0) { print("ls: cannot open /\n"); exit(); }

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
