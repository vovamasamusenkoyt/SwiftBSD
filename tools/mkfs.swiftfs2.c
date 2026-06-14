#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SWIFTFS2_MAGIC         0x3253465446495753ULL
#define SWIFTFS2_BLOCK_SIZE    4096
#define SWIFTFS2_BLOCKS_PER_GROUP   32768
#define SWIFTFS2_INODES_PER_GROUP   4096
#define SWIFTFS2_INODE_SIZE         128
#define SWIFTFS2_DIRECT_COUNT   12

#define S_IFDIR     0040000
#define S_IRWXU     00700

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
} __attribute__((packed)) super_t;

typedef struct {
    uint32_t block_bitmap_block;
    uint32_t inode_table_block;
    uint32_t free_blocks_count;
    uint32_t free_inodes_count;
    uint32_t pad[4];
} __attribute__((packed)) gd_t;

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
} __attribute__((packed)) inode_t;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
} __attribute__((packed)) dirent_t;

#define FILE_TYPE_DIR 2

static uint32_t bitmap_set_range(uint8_t *bm, uint32_t start, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        int byte = (start + i) / 8;
        int bit  = (start + i) % 8;
        bm[byte] |= (1 << bit);
    }
    return count;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <image> [size_mb]\n", argv[0]);
        return 1;
    }

    const char *img_path = argv[1];
    uint64_t size_mb = 128;
    if (argc >= 3) size_mb = atol(argv[2]);
    if (size_mb < 1) size_mb = 1;

    uint64_t num_blocks = (size_mb * 1024 * 1024) / SWIFTFS2_BLOCK_SIZE;
    int nr_groups = (num_blocks + SWIFTFS2_BLOCKS_PER_GROUP - 1)
                    / SWIFTFS2_BLOCKS_PER_GROUP;

    /* Layout per group:
       block 0 of group: block bitmap
       block 1..N of group: inode table (128 blocks for 4096 inodes)
       rest: data blocks
       Group 0: superblock occupies the first block (block 0 of disk),
                group descriptors follow (blocks 1..G),
                then block bitmap at block G+1 of group,
                then inode table, then data */
    int gd_per_block = SWIFTFS2_BLOCK_SIZE / sizeof(gd_t);
    int gd_blocks = (nr_groups + gd_per_block - 1) / gd_per_block;
    int journal_size_blocks = 8192; /* 32MB */

    /* Journal: last journal_size_blocks of the disk */
    uint64_t journal_block = num_blocks - journal_size_blocks;

    /* Create file and write zeros */
    FILE *f = fopen(img_path, "wb");
    if (!f) { perror(img_path); return 1; }

    uint8_t zero[SWIFTFS2_BLOCK_SIZE];
    memset(zero, 0, sizeof(zero));

    /* Pre-allocate the whole file */
    for (uint64_t i = 0; i < num_blocks; i++)
        fwrite(zero, SWIFTFS2_BLOCK_SIZE, 1, f);
    rewind(f);

    /* Write superblock */
    super_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = SWIFTFS2_MAGIC;
    sb.block_size = SWIFTFS2_BLOCK_SIZE;
    sb.num_blocks = num_blocks;
    sb.inode_count = (uint64_t)nr_groups * SWIFTFS2_INODES_PER_GROUP;
    sb.free_blocks = num_blocks;
    sb.free_inodes = sb.inode_count - 1; /* root inode consumes 1 */
    sb.blocks_per_group = SWIFTFS2_BLOCKS_PER_GROUP;
    sb.inodes_per_group = SWIFTFS2_INODES_PER_GROUP;
    sb.group_desc_count = gd_blocks;
    sb.inode_size = SWIFTFS2_INODE_SIZE;
    sb.journal_block = journal_block;
    sb.journal_size = journal_size_blocks;
    sb.journal_last = 0;
    memcpy(sb.uuid, "swiftfs2-v2-test", 16);
    memcpy(sb.volume_name, "root", 5);

    /* Write group descriptors */
    gd_t *gd = calloc(nr_groups, sizeof(gd_t));
    if (!gd) { fclose(f); return 1; }

    uint64_t reserved_blocks_group0 = 1 + gd_blocks + 1;
    /* 1 superblock + G group desc blocks + 1 bitmap block */
    uint32_t inode_table_blocks =
        (SWIFTFS2_INODES_PER_GROUP * SWIFTFS2_INODE_SIZE
         + SWIFTFS2_BLOCK_SIZE - 1) / SWIFTFS2_BLOCK_SIZE;

    uint64_t total_reserved = 0;

    for (int g = 0; g < nr_groups; g++) {
        uint32_t group_start = g * SWIFTFS2_BLOCKS_PER_GROUP;
        uint32_t bitmap_block = group_start;
        uint32_t inode_table_block = bitmap_block + 1;
        uint32_t data_start = inode_table_block + inode_table_blocks;

        if (g == 0) {
            /* Group 0: first block is superblock, then descriptors */
            bitmap_block = 1 + gd_blocks;
            inode_table_block = bitmap_block + 1;
            data_start = inode_table_block + inode_table_blocks;
        }

        gd[g].block_bitmap_block = bitmap_block;
        gd[g].inode_table_block = inode_table_block;
        gd[g].free_blocks_count = SWIFTFS2_BLOCKS_PER_GROUP;
        gd[g].free_inodes_count = SWIFTFS2_INODES_PER_GROUP;

        /* Mark bitmap block and inode table blocks as used */
        uint32_t used_blocks = (g == 0)
            ? (1 + gd_blocks + 1 + inode_table_blocks)
            : (1 + inode_table_blocks);
        gd[g].free_blocks_count -= used_blocks;
        uint32_t sb_sub = used_blocks;
        uint32_t fat_used = used_blocks;

        /* Mark journal blocks as used in last group */
        if (g == nr_groups - 1) {
            gd[g].free_blocks_count -= journal_size_blocks;
            fat_used += journal_size_blocks;
        }

        /* Subtract from global count */
        sb.free_blocks -= fat_used;

        /* Write block bitmap */
        uint8_t bm[SWIFTFS2_BLOCK_SIZE];
        memset(bm, 0, sizeof(bm));

        /* Mark reserved blocks as used (contiguous metadata at start) */
        bitmap_set_range(bm, 0, sb_sub);
        if (g == nr_groups - 1) {
            /* Mark journal blocks */
            uint32_t j_start = journal_block - group_start;
            bitmap_set_range(bm, j_start, journal_size_blocks);
        }

        fseek(f, (long)bitmap_block * SWIFTFS2_BLOCK_SIZE, SEEK_SET);
        fwrite(bm, SWIFTFS2_BLOCK_SIZE, 1, f);

        /* Zero out inode table (already zeroed by prealloc) */
    }

    /* Write superblock */
    fseek(f, 0, SEEK_SET);
    fwrite(&sb, sizeof(sb), 1, f);

    /* Write group descriptors */
    fseek(f, SWIFTFS2_BLOCK_SIZE, SEEK_SET);
    fwrite(gd, sizeof(gd_t), nr_groups, f);

    /* Create root inode (inode 1) */
    inode_t root_inode;
    memset(&root_inode, 0, sizeof(root_inode));
    root_inode.mode = S_IFDIR | 0755;
    root_inode.size = SWIFTFS2_BLOCK_SIZE; /* one block for . and .. */
    root_inode.block_count = 1;

    /* Allocate first data block for root dir */
    uint32_t root_data_block = 0;
    for (int g = 0; g < nr_groups; g++) {
        uint32_t start = g * SWIFTFS2_BLOCKS_PER_GROUP;
        uint32_t bitmap_block = gd[g].block_bitmap_block;
        uint8_t bm[SWIFTFS2_BLOCK_SIZE];
        fseek(f, (long)bitmap_block * SWIFTFS2_BLOCK_SIZE, SEEK_SET);
        fread(bm, SWIFTFS2_BLOCK_SIZE, 1, f);

        for (uint32_t i = 0; i < SWIFTFS2_BLOCKS_PER_GROUP; i++) {
            int byte = i / 8;
            int bit = i % 8;
            if (!(bm[byte] & (1 << bit))) {
                root_data_block = start + i;
                /* Mark as used */
                bm[byte] |= (1 << bit);
                fseek(f, (long)bitmap_block * SWIFTFS2_BLOCK_SIZE, SEEK_SET);
                fwrite(bm, SWIFTFS2_BLOCK_SIZE, 1, f);
                goto found_block;
            }
        }
    }
found_block:
    if (!root_data_block) {
        fprintf(stderr, "no free block for root dir\n");
        fclose(f);
        return 1;
    }

    root_inode.direct[0] = root_data_block;

    /* Write root inode to inode table of group 0, slot 1 (inode 1) */
    uint32_t root_ino_block = gd[0].inode_table_block;
    uint32_t inode_per_block = SWIFTFS2_BLOCK_SIZE / SWIFTFS2_INODE_SIZE;
    uint32_t root_ino_offset = 1 * SWIFTFS2_INODE_SIZE; /* inode 1, skip 0 */
    fseek(f, (long)root_ino_block * SWIFTFS2_BLOCK_SIZE + root_ino_offset, SEEK_SET);
    fwrite(&root_inode, sizeof(root_inode), 1, f);

    /* Write root directory entries: . and .. */
    uint8_t dir_block[SWIFTFS2_BLOCK_SIZE];
    memset(dir_block, 0, sizeof(dir_block));

    dirent_t *dot = (dirent_t *)dir_block;
    dot->inode = 1;
    dot->rec_len = sizeof(dirent_t) + 1 + 3;  /* ".", padded to 8 */
    dot->name_len = 1;
    dot->file_type = FILE_TYPE_DIR;
    dot->name[0] = '.';

    dirent_t *dotdot = (dirent_t *)(dir_block + dot->rec_len);
    dotdot->inode = 1; /* parent is self for root */
    dotdot->rec_len = sizeof(dirent_t) + 2 + 2;  /* "..", padded to 8 */
    dotdot->name_len = 2;
    dotdot->file_type = FILE_TYPE_DIR;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';

    dirent_t *end = (dirent_t *)(dir_block + dot->rec_len + dotdot->rec_len);
    end->inode = 0;
    end->rec_len = SWIFTFS2_BLOCK_SIZE - dot->rec_len - dotdot->rec_len;

    fseek(f, (long)root_data_block * SWIFTFS2_BLOCK_SIZE, SEEK_SET);
    fwrite(dir_block, SWIFTFS2_BLOCK_SIZE, 1, f);

    /* Update group 0 free counts */
    gd[0].free_inodes_count--; /* root inode */
    fseek(f, SWIFTFS2_BLOCK_SIZE + 0 * sizeof(gd_t), SEEK_SET);
    fwrite(&gd[0], sizeof(gd_t), 1, f);

    fclose(f);
    printf("Created %s: %llu MB, %llu blocks, %d groups, "
           "root inode=1, root data block=%u\n",
           img_path, size_mb, num_blocks, nr_groups, root_data_block);
    return 0;
}
