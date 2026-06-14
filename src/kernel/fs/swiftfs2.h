#pragma once
#include <stdint.h>

#define SWIFTFS2_MAGIC         0x3253465446495753ULL
#define SWIFTFS2_BLOCK_SIZE    4096
#define SWIFTFS2_BLOCK_SHIFT   12
#define SWIFTFS2_SECTOR_SHIFT  9
#define SWIFTFS2_SECTORS_PER_BLOCK  8

#define SWIFTFS2_BLOCKS_PER_GROUP   32768
#define SWIFTFS2_INODES_PER_GROUP   4096
#define SWIFTFS2_INODE_SIZE         128
#define SWIFTFS2_INODES_PER_BLOCK   (SWIFTFS2_BLOCK_SIZE / SWIFTFS2_INODE_SIZE)

#define SWIFTFS2_DIRECT_COUNT   12
#define SWIFTFS2_INDIRECT_ENTS  (SWIFTFS2_BLOCK_SIZE / 4)

#define SWIFTFS2_MAX_FD         64
#define SWIFTFS2_CACHE_SIZE     64
#define SWIFTFS2_NAME_MAX       255
#define SWIFTFS2_FILE_TYPE_UNKNOWN 0
#define SWIFTFS2_FILE_TYPE_REG  1
#define SWIFTFS2_FILE_TYPE_DIR  2

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0x100
#define O_TRUNC     0x200

#define S_IFMT      0170000
#define S_IFREG     0100000
#define S_IFDIR     0040000
#define S_IRWXU     00700
#define S_IRUSR     00400
#define S_IWUSR     00200
#define S_IXUSR     00100
#define S_IRWXG     00070
#define S_IRGRP     00040
#define S_IWGRP     00020
#define S_IXGRP     00010
#define S_IRWXO     00007
#define S_IROTH     00004
#define S_IWOTH     00002
#define S_IXOTH     00001

/* --- On-disk structures --- */

typedef struct {
    uint64_t magic;
    uint64_t block_size;
    uint64_t num_blocks;
    uint64_t inode_count;
    uint64_t free_blocks;
    uint64_t free_inodes;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t group_desc_count;
    uint32_t inode_size;
    uint64_t journal_block;
    uint64_t journal_size;
    uint64_t journal_last;
    uint8_t  uuid[16];
    uint8_t  volume_name[32];
    uint8_t  pad[3960];
} __attribute__((packed)) swiftfs2_super_t;

typedef struct {
    uint32_t block_bitmap_block;
    uint32_t inode_table_block;
    uint32_t free_blocks_count;
    uint32_t free_inodes_count;
    uint32_t pad[4];
} __attribute__((packed)) swiftfs2_gd_t;

typedef struct {
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
    uint64_t size;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    uint32_t block_count;
    uint32_t direct[SWIFTFS2_DIRECT_COUNT];
    uint32_t indirect;
    uint32_t double_indirect;
    uint8_t  pad[20];
} __attribute__((packed)) swiftfs2_inode_t;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
} __attribute__((packed)) swiftfs2_dirent_t;

/* --- Mount state --- */

typedef struct {
    uint32_t inode;
    uint32_t pos;
    int      flags;
    int      dirty; /* inode metadata needs write-back */
} swiftfs2_fd_t;

/* --- Public API --- */

int swiftfs2_mount(int ahci_port);
int swiftfs2_open(const char *path, int flags);
int swiftfs2_read(int fd, void *buf, uint32_t size);
int swiftfs2_write(int fd, const void *buf, uint32_t size);
int swiftfs2_close(int fd);
int swiftfs2_mkdir(const char *path, uint16_t mode);
int swiftfs2_unlink(const char *path);
int swiftfs2_ls(const char *path,
                void (*cb)(const char *name, uint16_t mode, uint64_t size));
int swiftfs2_sync(void);
int swiftfs2_umount(void);