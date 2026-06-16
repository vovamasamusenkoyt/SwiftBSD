#pragma once
#include <stdint.h>

#define VFS_MAX_FILES 64
#define VFS_NAME_MAX  16
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

struct stat {
    unsigned short mode;
    unsigned long long size;
};

struct vfs_ops {
    char name[VFS_NAME_MAX];
    int  (*open) (const char *path, int flags, void **priv_out);
    int  (*read) (void *priv, void *buf, uint32_t sz, uint64_t *pos);
    int  (*write)(void *priv, const void *buf, uint32_t sz, uint64_t *pos);
    int  (*close)(void *priv);
    int  (*fstat)(void *priv, struct stat *st);
};

struct file {
    struct vfs_ops *ops;
    void *priv;
    uint64_t pos;
    int flags;
    int refcount;
};

void vfs_init(struct file *fds);
void vfs_set_fds(struct file *fds);
int  vfs_register(struct vfs_ops *ops);
int  vfs_open(const char *path, int flags);
int  vfs_read(int fd, void *buf, uint32_t sz);
int  vfs_write(int fd, const void *buf, uint32_t sz);
int  vfs_close(int fd);
int  vfs_fstat(int fd, struct stat *st);
int  vfs_lseek(int fd, int64_t offset, int whence);
int  vfs_get_file(int fd, struct file **f);
int  vfs_pipe(int fds[2]);
int  vfs_dup(int oldfd);
int  vfs_dup2(int oldfd, int newfd);
