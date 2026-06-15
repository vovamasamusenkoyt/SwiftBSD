#include "syscall.h"
#include <stdint.h>

#define MAX_LINE 256
#define HISTORY_MAX 16

static char line[MAX_LINE];
static char cwd[256] = "/";

static char history[HISTORY_MAX][MAX_LINE];
static int history_count;

static void history_add(const char *buf, int len) {
    int j;
    for (int i = HISTORY_MAX - 1; i > 0; i--) {
        for (j = 0; j < MAX_LINE - 1 && history[i-1][j]; j++)
            history[i][j] = history[i-1][j];
        history[i][j] = 0;
    }
    for (j = 0; j < len && j < MAX_LINE - 1; j++)
        history[0][j] = buf[j];
    history[0][j] = 0;
    if (history_count < HISTORY_MAX) history_count++;
}

static void print_prompt(void) {
    print("[user@swiftbsd ");
    print(cwd);
    print("]$ ");
}

static int prompt_len(void) {
    return 16 + strlen(cwd) + 3;
}

static int readline(char *buf, int max) {
    int i = 0;
    int hb = -1;
    char saved[MAX_LINE];
    saved[0] = 0;

    while (i < max - 1) {
        char c;
        if (read(0, &c, 1) <= 0) break;

        if (c == '\n' || c == '\r') {
            buf[i] = 0;
            print("\n");
            if (i > 0) history_add(buf, i);
            return i;
        }

        if (c == '\b' || c == 127) {
            if (i > 0) { i--; print("\b \b"); }
            hb = -1;
            continue;
        }

        if (c == 0x1B) {
            char seq[2];
            if (read(0, seq, 2) == 2 && seq[0] == '[') {
                if (seq[1] == 'A') {
                    if (hb == -1) {
                        int j;
                        for (j = 0; j < i && j < MAX_LINE; j++) saved[j] = buf[j];
                        saved[j] = 0;
                        if (history_count > 0) hb = 0;
                        else continue;
                    } else if (hb < history_count - 1) {
                        hb++;
                    } else {
                        continue;
                    }
                } else if (seq[1] == 'B') {
                    if (hb > 0) {
                        hb--;
                    } else if (hb == 0) {
                        hb = -1;
                        print("\r");
                        for (int j = 0; j < prompt_len() + i; j++) print(" ");
                        print("\r");
                        print_prompt();
                        for (int j = 0; saved[j]; j++) {
                            buf[j] = saved[j];
                            { char ec[2] = {saved[j], 0}; print(ec); }
                        }
                        i = strlen(saved);
                        continue;
                    } else {
                        continue;
                    }
                } else {
                    continue;
                }

                int j;
                for (j = 0; j < prompt_len() + i; j++) print(" ");
                print("\r");
                print_prompt();
                const char *hent = history[hb];
                for (j = 0; hent[j]; j++) {
                    buf[j] = hent[j];
                    { char ec[2] = {hent[j], 0}; print(ec); }
                }
                i = strlen(hent);
            }
            continue;
        }

        buf[i++] = c;
        { char ec[2] = {c, 0}; print(ec); }
        hb = -1;
    }

    buf[i] = 0;
    return i;
}

static void print_dec(uint64_t n) {
    char buf[21];
    int pos = 20;
    buf[20] = 0;
    if (n == 0) { print("0"); return; }
    while (n > 0) {
        buf[--pos] = '0' + (n % 10);
        n /= 10;
    }
    print(buf + pos);
}

static void normalize_path(const char *rel, char *out) {
    char buf[256];
    int pos = 0;

    if (!rel || !*rel) {
        out[0] = '/'; out[1] = 0;
        return;
    }

    if (rel[0] == '/') {
        buf[0] = '/'; pos = 1;
        rel++;
    } else {
        int j;
        for (j = 0; cwd[j] && j < 255; j++) buf[j] = cwd[j];
        pos = j;
    }

    while (*rel) {
        while (*rel == '/') rel++;
        if (!*rel) break;

        char comp[256];
        int ci = 0;
        while (rel[ci] && rel[ci] != '/' && ci < 255) {
            comp[ci] = rel[ci];
            ci++;
        }
        comp[ci] = 0;
        rel += ci;

        if (ci == 1 && comp[0] == '.') {
        } else if (ci == 2 && comp[0] == '.' && comp[1] == '.') {
            if (pos > 1) {
                pos--;
                while (pos > 0 && buf[pos-1] != '/') pos--;
            }
        } else {
            int need = (pos > 0 && buf[pos-1] != '/');
            if (pos + need + ci >= 255) break;
            if (need) buf[pos++] = '/';
            for (int j = 0; j < ci; j++) buf[pos++] = comp[j];
        }
    }

    buf[pos] = 0;
    for (int j = 0; j <= pos && j < 255; j++) out[j] = buf[j];
    out[255] = 0;
}

void _start(void) {
    print("\nSwiftBSD shell v0.1\n");

    while (1) {
        print_prompt();
        int n = readline(line, MAX_LINE);
        if (n == 0) continue;

        const char *p = line;
        while (*p == ' ') p++;
        if (*p == 0) continue;

        const char *cmd_start = p;
        while (*p && *p != ' ') p++;
        int cmd_len = p - cmd_start;

        while (*p == ' ') p++;
        const char *args = p;

        if (cmd_len == 4 && strncmp(cmd_start, "exit", 4) == 0) { _exit(0); }
        if (cmd_len == 4 && strncmp(cmd_start, "halt", 4) == 0) { halt(); }
        if (cmd_len == 4 && strncmp(cmd_start, "quit", 4) == 0) { _exit(0); }

        if (cmd_len == 2 && strncmp(cmd_start, "cd", 2) == 0) {
            char buf[256];
            normalize_path(args, buf);
            int j;
            for (j = 0; buf[j]; j++) cwd[j] = buf[j];
            cwd[j] = 0;
            continue;
        }

        if ((cmd_len == 4 && strncmp(cmd_start, "free", 4) == 0) ||
            (cmd_len == 3 && strncmp(cmd_start, "mem", 3) == 0)) {
            uint64_t info[2];
            syscall(SC_MEMINFO, (long)info, 0, 0);
            uint64_t total = info[0];
            uint64_t free_pages = info[1];
            print("Mem:  ");
            print_dec(total / 1024);
            print(" KB total,  ");
            print_dec(free_pages * 4);
            print(" KB free (");
            print_dec(free_pages);
            print(" pages)\n");
            continue;
        }

        if (cmd_len == 2 && strncmp(cmd_start, "ls", 2) == 0 && *args == 0)
            args = cwd;

        char path[64];
        int i;
        for (i = 0; i < 5; i++) path[i] = "/bin/"[i];
        for (i = 0; i < cmd_len && i < 56; i++) path[5 + i] = cmd_start[i];
        path[5 + cmd_len] = 0;

        for (i = 0; args[i] && i < 255; i++) ARGS_PAGE[i] = args[i];
        ARGS_PAGE[i] = 0;

        int pid = fork();
        if (pid < 0) {
            print("fork failed\n");
            continue;
        }
        if (pid == 0) {
            /* Child: exec the command */
            int ret = exec(path, ARGS_PAGE);
            if (ret < 0) {
                print("? unknown cmd (try: ls, cat, echo, exit, halt, cd, free, mem)\n");
            }
            _exit(1);
        }
        /* Parent: wait for child */
        int status;
        int wpid = wait(&status);
        if (wpid < 0) {
            print("wait failed\n");
        }
    }

    _exit(0);
}
