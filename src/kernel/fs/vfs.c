#include "vfs.h"
#include "kernel.h"

static struct file files[VFS_MAX_FILES];
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

/* ── VFS init ── */

void vfs_init(void) {
    for (int i = 0; i < VFS_MAX_FILES; i++)
        files[i].used = 0;

    /* Pre-open fd 0 (stdin) and fd 1 (stdout) */
    files[0].ops   = &serial_ops;
    files[0].priv  = 0;
    files[0].pos   = 0;
    files[0].flags = 0;
    files[0].used  = 1;

    files[1].ops   = &serial_ops;
    files[1].priv  = 0;
    files[1].pos   = 0;
    files[1].flags = 0;
    files[1].used  = 1;
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
        if (!files[i].used) return i;
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
            files[fd].ops   = fs_list[i];
            files[fd].priv  = priv;
            files[fd].pos   = 0;
            files[fd].flags = flags;
            files[fd].used  = 1;
            return fd;
        }
    }
    return -1;
}

/* ── vfs_read / vfs_write ── */

int vfs_read(int fd, void *buf, uint32_t sz) {
    if (fd < 0 || fd >= VFS_MAX_FILES || !files[fd].used)
        return -1;
    return files[fd].ops->read(files[fd].priv, buf, sz, &files[fd].pos);
}

int vfs_write(int fd, const void *buf, uint32_t sz) {
    if (fd < 0 || fd >= VFS_MAX_FILES || !files[fd].used)
        return -1;
    return files[fd].ops->write(files[fd].priv, buf, sz, &files[fd].pos);
}

/* ── vfs_close ── */

int vfs_close(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FILES || !files[fd].used)
        return -1;
    int ret = files[fd].ops->close(files[fd].priv);
    files[fd].used = 0;
    files[fd].ops  = 0;
    files[fd].priv = 0;
    return ret;
}

/* ── vfs_fstat ── */

int vfs_fstat(int fd, struct stat *st) {
    if (fd < 0 || fd >= VFS_MAX_FILES || !files[fd].used)
        return -1;
    return files[fd].ops->fstat(files[fd].priv, st);
}

/* ── vfs_lseek ── */

int vfs_lseek(int fd, int64_t offset, int whence) {
    if (fd < 0 || fd >= VFS_MAX_FILES || !files[fd].used)
        return -1;
    uint64_t new_pos;
    switch (whence) {
    case SEEK_SET: new_pos = (uint64_t)offset; break;
    case SEEK_CUR: new_pos = files[fd].pos + (uint64_t)offset; break;
    default:       return -1;
    }
    files[fd].pos = new_pos;
    return (int)new_pos;
}

/* ── vfs_get_file (for mmap) ── */

int vfs_get_file(int fd, struct file **f) {
    if (fd < 0 || fd >= VFS_MAX_FILES || !files[fd].used)
        return -1;
    *f = &files[fd];
    return 0;
}
