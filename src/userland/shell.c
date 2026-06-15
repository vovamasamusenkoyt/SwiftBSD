#include "syscall.h"
#include <stdint.h>

#define MAX_LINE 256

static char line[MAX_LINE];

static int readline(char *buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char c;
        if (read(0, &c, 1) <= 0) break;
        if (c == '\n' || c == '\r') { buf[i] = 0; print("\n"); return i; }
        if (c == '\b' || c == 127) {
            if (i > 0) { i--; print("\b \b"); }
            continue;
        }
        buf[i++] = c;
        { char ec[2] = {c, 0}; print(ec); }
    }
    buf[i] = 0;
    return i;
}

/* built-in ls: list root directory */
static void cmd_ls(void) {
    uint8_t raw[4096];
    int fd = open("/", O_RDONLY);
    if (fd < 0) { print("ls: cannot open /\n"); return; }
    int n = read(fd, raw, sizeof(raw));
    close(fd);
    if (n <= 0) return;

    int pos = 0;
    while (pos < n) {
        uint32_t ino = *(uint32_t *)(raw + pos);
        if (ino == 0) break;
        uint16_t reclen = *(uint16_t *)(raw + pos + 4);
        uint8_t nlen = *(uint8_t *)(raw + pos + 6);
        uint8_t ftype = *(uint8_t *)(raw + pos + 7);
        if (reclen == 0 || nlen == 0) break;

        char name[256];
        int i;
        for (i = 0; i < nlen && i < 255; i++)
            name[i] = raw[pos + 8 + i];
        name[i] = 0;

        if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) {
            pos += reclen;
            continue; /* skip . and .. */
        }

        print(ftype == 2 ? "d " : "- ");
        print(name);
        print("\n");
        pos += reclen;
    }
}

/* built-in cat: print file */
static void cmd_cat(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { print("cat: cannot open "); print(path); print("\n"); return; }
    char buf[256];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);
    close(fd);
}

/* built-in echo: print arguments */
static void cmd_echo(const char *args) {
    print(args);
    print("\n");
}

void _start(void) {
    print("\nSwiftBSD shell v0.1\n");

    while (1) {
        print("$ ");
        int n = readline(line, MAX_LINE);
        if (n == 0) continue;

        /* Parse command */
        const char *args = line;
        while (*args == ' ') args++;

        if (strcmp(args, "exit") == 0 || strcmp(args, "quit") == 0)
            break;
        else if (strcmp(args, "ls") == 0)
            cmd_ls();
        else if (args[0] == 'c' && args[1] == 'a' && args[2] == 't' && args[3] == ' ')
            cmd_cat(args + 4);
        else if (args[0] == 'e' && args[1] == 'c' && args[2] == 'h' && args[3] == 'o' && args[4] == ' ')
            cmd_echo(args + 5);
        else
            print("? unknown cmd (ls/cat/echo/exit)\n");
    }

    exit();
}