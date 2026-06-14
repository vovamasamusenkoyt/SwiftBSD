#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#define SWIFTFS_MAGIC   0x53465753
#define SWIFTFS_BLOCK   512
#define SWIFTFS_NAME    24
#define MAX_FILES       64

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t block_size;
    uint32_t dir_blocks;
    uint32_t data_block;
    uint32_t total_blocks;
    uint8_t  reserved[492];
} __attribute__((packed)) super_t;

typedef struct {
    char     name[SWIFTFS_NAME];
    uint32_t size;
    uint32_t first_block;
} __attribute__((packed)) dirent_t;

static int add_file(const char *path, const char *name,
                    dirent_t *ents, int *nents,
                    uint32_t *next_block)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint32_t blocks = (sz + SWIFTFS_BLOCK - 1) / SWIFTFS_BLOCK;

    dirent_t *e = &ents[*nents];
    strncpy(e->name, name, SWIFTFS_NAME - 1);
    e->name[SWIFTFS_NAME - 1] = 0;
    e->size = sz;
    e->first_block = *next_block;

    uint8_t buf[SWIFTFS_BLOCK];
    for (uint32_t i = 0; i < blocks; i++) {
        memset(buf, 0, SWIFTFS_BLOCK);
        long remain = sz - i * SWIFTFS_BLOCK;
        if (remain > SWIFTFS_BLOCK) remain = SWIFTFS_BLOCK;
        if (remain > 0)
            fread(buf, remain, 1, f);
        fwrite(buf, SWIFTFS_BLOCK, 1, stdout);
    }

    fclose(f);
    (*nents)++;
    *next_block += blocks;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <fs_name>=<file_path> ...\n", argv[0]);
        return 1;
    }

    int nfiles = argc - 1;
    dirent_t entries[MAX_FILES];
    int nents = 0;

    super_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic       = SWIFTFS_MAGIC;
    sb.version     = 1;
    sb.block_size  = SWIFTFS_BLOCK;

    uint32_t dir_blocks = (nfiles * sizeof(dirent_t) + SWIFTFS_BLOCK - 1) / SWIFTFS_BLOCK;
    sb.dir_blocks  = dir_blocks;
    sb.data_block  = 1 + dir_blocks;
    sb.total_blocks = sb.data_block; /* will be updated */

    uint32_t next_block = sb.data_block;

    /* Write superblock (placeholder, will seek back) */
    long sb_pos = ftell(stdout);
    fwrite(&sb, sizeof(sb), 1, stdout);

    /* Reserve directory blocks */
    uint8_t zero[SWIFTFS_BLOCK];
    memset(zero, 0, SWIFTFS_BLOCK);
    for (uint32_t i = 0; i < dir_blocks; i++)
        fwrite(zero, SWIFTFS_BLOCK, 1, stdout);

    /* Write files */
    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];
        char *eq = strchr(arg, '=');
        if (!eq) {
            fprintf(stderr, "bad arg: %s (need name=path)\n", arg);
            return 1;
        }
        *eq = 0;
        const char *name = arg;
        const char *path = eq + 1;
        if (add_file(path, name, entries, &nents, &next_block) < 0)
            return 1;
    }

    /* Update superblock */
    sb.total_blocks = next_block;
    fseek(stdout, sb_pos, SEEK_SET);
    fwrite(&sb, sizeof(sb), 1, stdout);

    /* Write directory at right position */
    fseek(stdout, sizeof(sb), SEEK_SET);
    fwrite(entries, sizeof(dirent_t), nents, stdout);

    return 0;
}
