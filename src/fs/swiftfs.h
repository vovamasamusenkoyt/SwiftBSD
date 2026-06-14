#pragma once
#include <stdint.h>

#define SWIFTFS_MAGIC   0x53465753
#define SWIFTFS_BLOCK   512
#define SWIFTFS_NAME    24

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t block_size;
    uint32_t dir_blocks;
    uint32_t data_block;
    uint32_t total_blocks;
    uint8_t  reserved[492];
} __attribute__((packed)) swiftfs_super_t;

typedef struct {
    char     name[SWIFTFS_NAME];
    uint32_t size;
    uint32_t first_block;
} __attribute__((packed)) swiftfs_dirent_t;

#define SWIFTFS_DIRENTS_PER_BLOCK (SWIFTFS_BLOCK / sizeof(swiftfs_dirent_t))

int swiftfs_mount(int port);
int swiftfs_read(const char *name, void *buf, uint32_t size);
