#include "swiftfs.h"
#include "ahci.h"
#include "kernel.h"
#include "string.h"

#define MAX_OPEN_FILES 16

typedef struct {
    char     name[SWIFTFS_NAME];
    uint32_t size;
    uint32_t first_block;
} mount_file_t;

static mount_file_t files[MAX_OPEN_FILES];
static int nfiles;
static int mounted;
static int fs_port;
static uint32_t fs_data_block;

static int read_blocks(int port, uint32_t lba, void *buf, int count) {
    return ahci_read(port, lba, buf, count);
}

int swiftfs_mount(int port) {
    uint8_t block[SWIFTFS_BLOCK];
    int ret = read_blocks(port, 0, block, 1);
    if (ret < 0) return -1;

    swiftfs_super_t *sb = (swiftfs_super_t *)block;
    if (sb->magic != SWIFTFS_MAGIC) {
        serial_printf("[swiftfs] bad magic: %x\n", (unsigned)sb->magic);
        return -1;
    }

    if (sb->version != 1 || sb->block_size != SWIFTFS_BLOCK) {
        serial_printf("[swiftfs] unsupported version=%d block=%d\n",
                      sb->version, sb->block_size);
        return -1;
    }

    fs_port = port;
    fs_data_block = sb->data_block;

    uint32_t dir_bytes = sb->dir_blocks * SWIFTFS_BLOCK;
    uint8_t *dir_buf = kmalloc(dir_bytes);
    if (!dir_buf) return -1;

    ret = read_blocks(port, 1, dir_buf, sb->dir_blocks);
    if (ret < 0) {
        kfree(dir_buf);
        return -1;
    }

    swiftfs_dirent_t *de = (swiftfs_dirent_t *)dir_buf;
    int max_ents = dir_bytes / sizeof(swiftfs_dirent_t);

    nfiles = 0;
    for (int i = 0; i < max_ents && nfiles < MAX_OPEN_FILES; i++) {
        if (de[i].name[0] == 0) break;
        memcpy(files[nfiles].name, de[i].name, SWIFTFS_NAME);
        files[nfiles].size = de[i].size;
        files[nfiles].first_block = de[i].first_block;
        nfiles++;
    }

    kfree(dir_buf);
    mounted = 1;
    serial_printf("[swiftfs] mounted: %d files\n", nfiles);
    for (int i = 0; i < nfiles; i++)
        serial_printf("  %s (%d bytes, block %d)\n",
                      files[i].name, files[i].size, files[i].first_block);
    return 0;
}

int swiftfs_read(const char *name, void *buf, uint32_t size) {
    if (!mounted) return -1;

    mount_file_t *f = 0;
    for (int i = 0; i < nfiles; i++) {
        if (strcmp(files[i].name, name) == 0) {
            f = &files[i];
            break;
        }
    }
    if (!f) return -1;

    uint32_t to_read = size;
    if (to_read > f->size) to_read = f->size;
    if (to_read == 0) return 0;

    uint32_t block = f->first_block;
    uint32_t offset = 0;
    uint32_t remain = to_read;

    while (remain > 0) {
        uint32_t chunk = remain;
        if (chunk > SWIFTFS_BLOCK) chunk = SWIFTFS_BLOCK;

        uint8_t tmp[SWIFTFS_BLOCK];
        int ret = read_blocks(fs_port, block, tmp, 1);
        if (ret < 0) return -1;

        memcpy((uint8_t *)buf + offset, tmp, chunk);
        offset += chunk;
        remain -= chunk;
        block++;
    }

    return to_read;
}
