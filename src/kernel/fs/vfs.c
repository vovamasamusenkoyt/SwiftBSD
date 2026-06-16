#include "vfs.h"
#include "kernel.h"
#include "string.h"
#include "sched.h"

static struct file *g_fds;
static struct vfs_ops *fs_list[8];
static int nr_fs;

/* ── serial ops for stdin/stdout ── */

static int ser_open(const char *path, int flags, void **priv) {
    (void)path; (void)flags; (void)priv;
    return 0;
}

static int ser_read(void *priv, void *buf, uint32_t sz, uint64_t *pos) {
    (void)priv; (void)pos;
    for (uint32_t i = 0; i < sz; i++)
        ((char *)buf)[i] = serial_getc();
    return (int)sz;
}

static int ser_write(void *priv, const void *buf, uint32_t sz, uint64_t *pos) {
    (void)priv; (void)pos;
    for (uint32_t i = 0; i < sz; i++)
        serial_putc(((const char *)buf)[i]);
    return (int)sz;
}

static int ser_close(void *priv) {
    (void)priv;
    return 0;
}

static int ser_fstat(void *priv, struct stat *st) {
    (void)priv; (void)st;
    return -1;
}

static struct vfs_ops serial_ops = {
    .name  = "serial",
    .open  = ser_open,
    .read  = ser_read,
    .write = ser_write,
    .close = ser_close,
    .fstat = ser_fstat,
};

/* ── pipe internals ── */

#define PIPE_BUF_SIZE 4096

struct pipe {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t rpos, wpos;
    int readers, writers;
};

static int pipe_read_op(void *priv, void *buf, uint32_t sz, uint64_t *pos) {
    (void)pos;
    struct pipe *p = (struct pipe *)priv;
    uint32_t total = 0;
    while (sz > 0) {
        uint32_t avail = p->wpos - p->rpos;
        if (avail == 0) {
            if (p->writers == 0) return (int)total;
            sched_yield();
            continue;
        }
        uint32_t n = sz < avail ? sz : avail;
        uint32_t off = p->rpos % PIPE_BUF_SIZE;
        if (off + n <= PIPE_BUF_SIZE) {
            memcpy(buf, p->buf + off, n);
        } else {
            uint32_t first = PIPE_BUF_SIZE - off;
            memcpy(buf, p->buf + off, first);
            memcpy((uint8_t *)buf + first, p->buf, n - first);
        }
        p->rpos += n;
        buf = (uint8_t *)buf + n;
        sz -= n;
        total += n;
    }
    return (int)total;
}

static int pipe_write_op(void *priv, const void *buf, uint32_t sz, uint64_t *pos) {
    (void)pos;
    struct pipe *p = (struct pipe *)priv;
    uint32_t total = 0;
    while (sz > 0) {
        uint32_t space = PIPE_BUF_SIZE - (p->wpos - p->rpos);
        if (space == 0) {
            if (p->readers == 0) return total > 0 ? (int)total : -1;
            sched_yield();
            continue;
        }
        uint32_t n = sz < space ? sz : space;
        uint32_t off = p->wpos % PIPE_BUF_SIZE;
        if (off + n <= PIPE_BUF_SIZE) {
            memcpy(p->buf + off, buf, n);
        } else {
            uint32_t first = PIPE_BUF_SIZE - off;
            memcpy(p->buf + off, buf, first);
            memcpy(p->buf, (uint8_t *)buf + first, n - first);
        }
        p->wpos += n;
        buf = (const uint8_t *)buf + n;
        sz -= n;
        total += n;
    }
    return (int)total;
}

static int pipe_close_op(void *priv) {
    struct pipe *p = (struct pipe *)priv;
    p->readers--;
    if (p->readers == 0 && p->writers == 0)
        kfree(p);
    return 0;
}

static int pipe_write_close_op(void *priv) {
    struct pipe *p = (struct pipe *)priv;
    p->writers--;
    if (p->readers == 0 && p->writers == 0)
        kfree(p);
    return 0;
}

static struct vfs_ops pipe_read_ops = {
    .name  = "pipe_r",
    .read  = pipe_read_op,
    .close = pipe_close_op,
};

static struct vfs_ops pipe_write_ops = {
    .name  = "pipe_w",
    .write = pipe_write_op,
    .close = pipe_write_close_op,
};

/* ── VFS init ── */

void vfs_init(struct file *fds) {
    g_fds = fds;
    for (int i = 0; i < VFS_MAX_FILES; i++)
        fds[i].refcount = 0;

    fds[0].ops      = &serial_ops;
    fds[0].refcount = 1;

    fds[1].ops      = &serial_ops;
    fds[1].refcount = 1;
}

void vfs_set_fds(struct file *fds) {
    g_fds = fds;
}

/* ── register a filesystem ── */

int vfs_register(struct vfs_ops *ops) {
    if (nr_fs >= (int)(sizeof(fs_list) / sizeof(fs_list[0])))
        return -1;
    fs_list[nr_fs++] = ops;
    return 0;
}

/* ── fd allocation ── */

static int fd_alloc(void) {
    for (int i = 2; i < VFS_MAX_FILES; i++)
        if (g_fds[i].refcount == 0) return i;
    return -1;
}

/* ── vfs_open ── */

int vfs_open(const char *path, int flags) {
    for (int i = 0; i < nr_fs; i++) {
        void *priv = 0;
        int ret = fs_list[i]->open(path, flags, &priv);
        if (ret == 0) {
            int fd = fd_alloc();
            if (fd < 0) {
                fs_list[i]->close(priv);
                return -1;
            }
            g_fds[fd].ops      = fs_list[i];
            g_fds[fd].priv     = priv;
            g_fds[fd].pos      = 0;
            g_fds[fd].flags    = flags;
            g_fds[fd].refcount = 1;
            return fd;
        }
    }
    return -1;
}

/* ── vfs_read / vfs_write ── */

int vfs_read(int fd, void *buf, uint32_t sz) {
    if (fd < 0 || fd >= VFS_MAX_FILES || g_fds[fd].refcount == 0)
        return -1;
    return g_fds[fd].ops->read(g_fds[fd].priv, buf, sz, &g_fds[fd].pos);
}

int vfs_write(int fd, const void *buf, uint32_t sz) {
    if (fd < 0 || fd >= VFS_MAX_FILES || g_fds[fd].refcount == 0)
        return -1;
    return g_fds[fd].ops->write(g_fds[fd].priv, buf, sz, &g_fds[fd].pos);
}

/* ── vfs_close ── */

int vfs_close(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FILES || g_fds[fd].refcount == 0)
        return -1;
    g_fds[fd].refcount--;
    if (g_fds[fd].refcount > 0) return 0;

    int ret = 0;
    if (g_fds[fd].ops->close)
        ret = g_fds[fd].ops->close(g_fds[fd].priv);
    g_fds[fd].ops      = 0;
    g_fds[fd].priv     = 0;
    g_fds[fd].pos      = 0;
    g_fds[fd].flags    = 0;
    g_fds[fd].refcount = 0;
    return ret;
}

/* ── vfs_fstat ── */

int vfs_fstat(int fd, struct stat *st) {
    if (fd < 0 || fd >= VFS_MAX_FILES || g_fds[fd].refcount == 0)
        return -1;
    return g_fds[fd].ops->fstat(g_fds[fd].priv, st);
}

/* ── vfs_lseek ── */

int vfs_lseek(int fd, int64_t offset, int whence) {
    if (fd < 0 || fd >= VFS_MAX_FILES || g_fds[fd].refcount == 0)
        return -1;
    uint64_t new_pos;
    switch (whence) {
    case SEEK_SET: new_pos = (uint64_t)offset; break;
    case SEEK_CUR: new_pos = g_fds[fd].pos + (uint64_t)offset; break;
    default:       return -1;
    }
    g_fds[fd].pos = new_pos;
    return (int)new_pos;
}

/* ── vfs_get_file (for mmap) ── */

int vfs_get_file(int fd, struct file **f) {
    if (fd < 0 || fd >= VFS_MAX_FILES || g_fds[fd].refcount == 0)
        return -1;
    *f = &g_fds[fd];
    return 0;
}

/* ── vfs_pipe ── */

int vfs_pipe(int fds[2]) {
    struct pipe *p = kmalloc(sizeof(struct pipe));
    if (!p) return -1;
    p->rpos = p->wpos = 0;
    p->readers = 1;
    p->writers = 1;

    int rfd = fd_alloc();
    if (rfd < 0) { kfree(p); return -1; }
    int wfd = fd_alloc();
    if (wfd < 0) { g_fds[rfd].refcount = 0; kfree(p); return -1; }

    g_fds[rfd].ops      = &pipe_read_ops;
    g_fds[rfd].priv     = p;
    g_fds[rfd].pos      = 0;
    g_fds[rfd].flags    = 0;
    g_fds[rfd].refcount = 1;

    g_fds[wfd].ops      = &pipe_write_ops;
    g_fds[wfd].priv     = p;
    g_fds[wfd].pos      = 0;
    g_fds[wfd].flags    = 0;
    g_fds[wfd].refcount = 1;

    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}

/* ── vfs_dup / vfs_dup2 ── */

int vfs_dup(int oldfd) {
    if (oldfd < 0 || oldfd >= VFS_MAX_FILES || g_fds[oldfd].refcount == 0)
        return -1;
    int newfd = fd_alloc();
    if (newfd < 0) return -1;
    g_fds[newfd] = g_fds[oldfd];
    g_fds[newfd].refcount = 1;
    g_fds[oldfd].refcount++;
    return newfd;
}

int vfs_dup2(int oldfd, int newfd) {
    if (oldfd < 0 || oldfd >= VFS_MAX_FILES || g_fds[oldfd].refcount == 0)
        return -1;
    if (newfd < 0 || newfd >= VFS_MAX_FILES)
        return -1;
    if (newfd == oldfd) return newfd;
    if (g_fds[newfd].refcount > 0)
        vfs_close(newfd);
    g_fds[newfd] = g_fds[oldfd];
    g_fds[newfd].refcount = 1;
    g_fds[oldfd].refcount++;
    return newfd;
}
