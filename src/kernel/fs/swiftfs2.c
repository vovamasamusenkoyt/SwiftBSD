#include "swiftfs2.h"
#include "kernel.h"
#include "string.h"
#include "ahci.h"

/* ========== Static state ========== */

static swiftfs2_super_t sb;
static int g_port = -1;
static int g_mounted = 0;

static int g_nr_groups;
static swiftfs2_gd_t *g_gd;        /* array of group descriptors */
static uint8_t **g_bitmaps;        /* cached bitmap blocks (one per group) */
static int g_curr_group;           /* hint for next-fit allocator */

/* In-memory inode cache (for open files) */
static swiftfs2_fd_t g_fds[SWIFTFS2_MAX_FD];

/* Block cache */
typedef struct {
    uint32_t  block;
    int       group;
    uint8_t   data[SWIFTFS2_BLOCK_SIZE];
    int       dirty;
    int       valid;
} cache_entry_t;

static cache_entry_t g_cache[SWIFTFS2_CACHE_SIZE];
static int g_cache_next;   /* round-robin replacement */

/* Journal state (stub) */

/* ========== Helpers ========== */

static uint64_t fs_block_to_sector(uint64_t fs_block) {
    return fs_block * SWIFTFS2_SECTORS_PER_BLOCK;
}

static int read_block(uint64_t block, void *buf) {
    return ahci_read(g_port, fs_block_to_sector(block), buf,
                     SWIFTFS2_SECTORS_PER_BLOCK);
}

static int write_block(uint64_t block, const void *buf) {
    return ahci_write(g_port, fs_block_to_sector(block), buf,
                      SWIFTFS2_SECTORS_PER_BLOCK);
}

/* ========== Block cache ========== */

static int cache_lookup(uint32_t block, int group) {
    for (int i = 0; i < SWIFTFS2_CACHE_SIZE; i++) {
        if (g_cache[i].valid && g_cache[i].block == block
            && g_cache[i].group == group)
            return i;
    }
    return -1;
}

static uint8_t *cache_get(uint32_t block, int group) {
    int idx = cache_lookup(block, group);
    if (idx >= 0) return g_cache[idx].data;

    idx = g_cache_next;
    g_cache_next = (g_cache_next + 1) % SWIFTFS2_CACHE_SIZE;

    if (g_cache[idx].dirty)
        write_block(g_cache[idx].block, g_cache[idx].data);

    g_cache[idx].block = block;
    g_cache[idx].group = group;
    g_cache[idx].dirty = 0;
    g_cache[idx].valid = 1;
    read_block(block, g_cache[idx].data);
    return g_cache[idx].data;
}

static void cache_mark_dirty(uint32_t block, int group) {
    for (int i = 0; i < SWIFTFS2_CACHE_SIZE; i++) {
        if (g_cache[i].valid && g_cache[i].block == block
            && g_cache[i].group == group) {
            g_cache[i].dirty = 1;
            return;
        }
    }
}

static void cache_flush_all(void) {
    for (int i = 0; i < SWIFTFS2_CACHE_SIZE; i++) {
        if (g_cache[i].valid && g_cache[i].dirty) {
            write_block(g_cache[i].block, g_cache[i].data);
            g_cache[i].dirty = 0;
        }
    }
}

/* ========== Block allocator ========== */

static int group_of_block(uint64_t block) {
    return block / SWIFTFS2_BLOCKS_PER_GROUP;
}

static uint64_t block_in_group(uint64_t block) {
    return block % SWIFTFS2_BLOCKS_PER_GROUP;
}

static uint32_t group_start_block(int group) {
    return group * SWIFTFS2_BLOCKS_PER_GROUP;
}

/* Load bitmap for group into cache (returns pointer to cached data) */
static uint8_t *bitmap_for_group(int group) {
    if (group < 0 || group >= g_nr_groups) return 0;
    if (!g_bitmaps[group]) {
        g_bitmaps[group] = kmalloc(SWIFTFS2_BLOCK_SIZE);
        if (!g_bitmaps[group]) return 0;
        memset(g_bitmaps[group], 0, SWIFTFS2_BLOCK_SIZE);
        read_block(g_gd[group].block_bitmap_block, g_bitmaps[group]);
    }
    return g_bitmaps[group];
}

static void bitmap_sync(int group) {
    if (!g_bitmaps[group]) return;
    write_block(g_gd[group].block_bitmap_block, g_bitmaps[group]);
}

static void bit_set(uint8_t *bm, int bit) {
    bm[bit / 8] |= (1 << (bit % 8));
}

static int bit_test(const uint8_t *bm, int bit) {
    return (bm[bit / 8] >> (bit % 8)) & 1;
}

static void bit_clear(uint8_t *bm, int bit) {
    bm[bit / 8] &= ~(1 << (bit % 8));
}

static uint32_t block_alloc(void) {
    int start = g_curr_group;
    for (int gi = 0; gi < g_nr_groups; gi++) {
        int g = (start + gi) % g_nr_groups;
        if (g_gd[g].free_blocks_count == 0) continue;

        uint8_t *bm = bitmap_for_group(g);
        if (!bm) continue;

        for (uint32_t b = 0; b < SWIFTFS2_BLOCKS_PER_GROUP; b++) {
            if (!bit_test(bm, b)) {
                bit_set(bm, b);
                g_gd[g].free_blocks_count--;
                sb.free_blocks--;
                g_curr_group = g;
                cache_mark_dirty(g_gd[g].block_bitmap_block, 0);
                return group_start_block(g) + b;
            }
        }
    }
    return 0; /* out of space */
}

static void block_free(uint32_t block) {
    if (!block) return;
    int g = group_of_block(block);
    uint32_t b = block_in_group(block);
    uint8_t *bm = bitmap_for_group(g);
    if (!bm) return;
    if (!bit_test(bm, b)) return; /* double-free guard */
    bit_clear(bm, b);
    g_gd[g].free_blocks_count++;
    sb.free_blocks++;
    cache_mark_dirty(g_gd[g].block_bitmap_block, 0);
}

/* ========== Group descriptor helpers ========== */

static int load_group_descriptors(void) {
    g_nr_groups = (sb.num_blocks + SWIFTFS2_BLOCKS_PER_GROUP - 1)
                   / SWIFTFS2_BLOCKS_PER_GROUP;
    int desc_blocks = g_nr_groups * sizeof(swiftfs2_gd_t);
    desc_blocks = (desc_blocks + SWIFTFS2_BLOCK_SIZE - 1) / SWIFTFS2_BLOCK_SIZE;
    if (desc_blocks != (int)sb.group_desc_count) return -1;

    uint8_t *raw = kmalloc(desc_blocks * SWIFTFS2_BLOCK_SIZE);
    if (!raw) return -1;

    for (int i = 0; i < desc_blocks; i++)
        read_block(1 + i, raw + i * SWIFTFS2_BLOCK_SIZE);

    g_gd = (swiftfs2_gd_t *)raw;

    g_bitmaps = kmalloc(sizeof(uint8_t *) * g_nr_groups);
    if (!g_bitmaps) { kfree(raw); return -1; }
    memset(g_bitmaps, 0, sizeof(uint8_t *) * g_nr_groups);
    return 0;
}

/* ========== Inode read/write ========== */

static void inode_block_and_offset(uint32_t ino, uint32_t *block,
                                   uint32_t *offset) {
    int g = ino / SWIFTFS2_INODES_PER_GROUP;
    uint32_t local = ino % SWIFTFS2_INODES_PER_GROUP;
    uint32_t tbl_start = g_gd[g].inode_table_block;
    uint32_t entry_per_block = SWIFTFS2_INODES_PER_BLOCK;
    *block = tbl_start + local / entry_per_block;
    *offset = (local % entry_per_block) * SWIFTFS2_INODE_SIZE;
}

static int inode_read(uint32_t ino, swiftfs2_inode_t *inode) {
    uint32_t block, offset;
    inode_block_and_offset(ino, &block, &offset);
    uint8_t *data = cache_get(block, 0);
    if (!data) return -1;
    memcpy(inode, data + offset, sizeof(swiftfs2_inode_t));
    return 0;
}

static int inode_write(uint32_t ino, const swiftfs2_inode_t *inode) {
    uint32_t block, offset;
    inode_block_and_offset(ino, &block, &offset);
    uint8_t *data = cache_get(block, 0);
    if (!data) return -1;
    memcpy(data + offset, inode, sizeof(swiftfs2_inode_t));
    cache_mark_dirty(block, 0);
    return 0;
}

static uint32_t inode_alloc(uint16_t mode) {
    for (int gi = 0; gi < g_nr_groups; gi++) {
        if (g_gd[gi].free_inodes_count == 0) continue;

        uint8_t *bm = bitmap_for_group(gi);
        if (!bm) continue;

        /* Inode bitmap is stored in same block as block bitmap for simplicity.
           Inode bits start after the block bitmap bits.
           We use a dedicated inode bitmap block instead. For now: scan inode table
           in group to find a zero-mode inode. */
        uint32_t start_ino = gi * SWIFTFS2_INODES_PER_GROUP;
        uint32_t end_ino = start_ino + SWIFTFS2_INODES_PER_GROUP;
        for (uint32_t ino = start_ino; ino < end_ino; ino++) {
            if (ino == 0) continue; /* inode 0 is invalid */
            swiftfs2_inode_t in;
            inode_read(ino, &in);
            if (in.mode == 0) {
                memset(&in, 0, sizeof(in));
                in.mode = mode;
                inode_write(ino, &in);
                g_gd[gi].free_inodes_count--;
                sb.free_inodes--;
                return ino;
            }
        }
    }
    return 0;
}

static void inode_free(uint32_t ino) {
    if (!ino) return;
    swiftfs2_inode_t in;
    memset(&in, 0, sizeof(in));
    inode_write(ino, &in);
    int g = ino / SWIFTFS2_INODES_PER_GROUP;
    g_gd[g].free_inodes_count++;
    sb.free_inodes++;
}

/* ========== Block mapping (logical -> physical) ========== */

static uint32_t bmap_read(swiftfs2_inode_t *inode, uint32_t lblock,
                          int create) {
    /* direct */
    if (lblock < SWIFTFS2_DIRECT_COUNT) {
        if (inode->direct[lblock] == 0 && create)
            inode->direct[lblock] = block_alloc();
        return inode->direct[lblock];
    }

    lblock -= SWIFTFS2_DIRECT_COUNT;

    /* single indirect */
    if (lblock < SWIFTFS2_INDIRECT_ENTS) {
        if (inode->indirect == 0) {
            if (!create) return 0;
            inode->indirect = block_alloc();
            if (!inode->indirect) return 0;
            uint8_t *z = cache_get(inode->indirect, 0);
            memset(z, 0, SWIFTFS2_BLOCK_SIZE);
            cache_mark_dirty(inode->indirect, 0);
        }
        uint32_t *tbl = (uint32_t *)cache_get(inode->indirect, 0);
        if (tbl[lblock] == 0 && create) {
            tbl[lblock] = block_alloc();
            cache_mark_dirty(inode->indirect, 0);
        }
        return tbl[lblock];
    }

    return 0; /* double indirect not yet implemented */
}

/* ========== Path walk ========== */

/* Tokenise /a/b/c: returns pointer after next slash, copies name into buf */
static const char *path_next(const char *path, char *buf, int *len) {
    if (!path) return 0;
    while (*path == '/') path++;
    if (!*path) return 0;
    int i = 0;
    while (path[i] && path[i] != '/' && i < SWIFTFS2_NAME_MAX - 1) {
        buf[i] = path[i];
        i++;
    }
    buf[i] = 0;
    *len = i;
    if (!path[i]) return path + i;
    return path + i;
}

static int path_walk(const char *path, uint32_t *inode_out) {
    uint32_t ino = 1; /* root inode */
    if (!path || *path == 0) { *inode_out = ino; return 0; }

    swiftfs2_inode_t in;
    char component[SWIFTFS2_NAME_MAX];
    int clen;

    path = path_next(path, component, &clen);
    if (!path && clen == 0) { *inode_out = ino; return 0; }

    while (1) {
        if (inode_read(ino, &in) < 0) {
            return -1;
        }
        if (!(in.mode & S_IFDIR)) {
            return -1;
        }

        /* Scan directory entries */
        int found = 0;
        for (uint32_t bi = 0; ; bi++) {
            uint32_t pblock = bmap_read(&in, bi, 0);
            if (!pblock) break;
            uint8_t *data = cache_get(pblock, 0);
            uint32_t pos = 0;
            while (pos < SWIFTFS2_BLOCK_SIZE) {
                swiftfs2_dirent_t *de = (swiftfs2_dirent_t *)(data + pos);
                if (de->inode == 0) break;
                if (de->rec_len == 0) break;
                if (de->name_len == (uint8_t)clen
                    && strncmp(de->name, component, clen) == 0) {
                    ino = de->inode;
                    found = 1;
                    break;
                }
                pos += de->rec_len;
            }
            if (found) break;
        }
        if (!found) {
            return -1;
        }

        path = path_next(path, component, &clen);
        if (!path) break;
    }

    *inode_out = ino;
    return 0;
}

/* ========== Journal (stub - no-op, write-through) ========== */

static int journal_begin(void) {
    return 0;
}

static int journal_commit(void) {
    return 0;
}

static void journal_recover(void) {
    (void)sb;
}

/* ========== Forward declarations ========== */
static int dir_add_entry(uint32_t dir_ino, const char *name,
                         uint32_t child_ino, uint8_t file_type);

/* ========== File operations ========== */

static int fd_alloc(void) {
    for (int i = 0; i < SWIFTFS2_MAX_FD; i++) {
        if (g_fds[i].inode == 0) return i;
    }
    return -1;
}

int swiftfs2_open(const char *path, int flags) {
    if (!g_mounted) return -1;

    uint32_t ino;
    if (path_walk(path, &ino) < 0) {
        if (!(flags & O_CREAT)) return -1;

        /* Create file: find parent dir, create entry */
        char parent_path[SWIFTFS2_NAME_MAX * 2];
        char fname[SWIFTFS2_NAME_MAX];
        uint32_t plen = strlen(path);
        if (plen >= SWIFTFS2_NAME_MAX * 2) plen = SWIFTFS2_NAME_MAX * 2 - 1;
        memcpy(parent_path, path, plen);
        parent_path[plen] = 0;

        char *slash = 0;
        for (char *p = parent_path; *p; p++)
            if (*p == '/') slash = p;

        uint32_t parent_ino;
        if (slash) {
            *slash = 0;
            if (path_walk(parent_path, &parent_ino) < 0) {
                return -1;
            }
            memcpy(fname, slash + 1, SWIFTFS2_NAME_MAX);
        } else {
            parent_ino = 1;
            memcpy(fname, path, SWIFTFS2_NAME_MAX);
        }

        ino = inode_alloc(S_IFREG | 0644);
        if (!ino) return -1;

        if (dir_add_entry(parent_ino, fname, ino, SWIFTFS2_FILE_TYPE_REG) < 0) {
            inode_free(ino);
            return -1;
        }
    }

    swiftfs2_inode_t in;
    inode_read(ino, &in);

    if ((flags & (O_WRONLY | O_RDWR)) && !(in.mode & S_IWUSR))
        return -1;

    if (flags & O_TRUNC) {
        for (int i = 0; i < SWIFTFS2_DIRECT_COUNT; i++) {
            if (in.direct[i]) { block_free(in.direct[i]); in.direct[i] = 0; }
        }
        if (in.indirect) { block_free(in.indirect); in.indirect = 0; }
        if (in.double_indirect) { block_free(in.double_indirect); in.double_indirect = 0; }
        in.size = 0;
        in.block_count = 0;
        inode_write(ino, &in);
    }

    int fd = fd_alloc();
    if (fd < 0) return -1;

    g_fds[fd].inode = ino;
    g_fds[fd].pos = 0;
    g_fds[fd].flags = flags;
    g_fds[fd].dirty = 0;
    return fd;
}

int swiftfs2_read(int fd, void *buf, uint32_t size) {
    if (fd < 0 || fd >= SWIFTFS2_MAX_FD || !g_fds[fd].inode) return -1;
    if (g_fds[fd].flags == O_WRONLY) return -1;

    swiftfs2_inode_t in;
    inode_read(g_fds[fd].inode, &in);

    uint32_t pos = g_fds[fd].pos;
    if (pos >= in.size) return 0;
    uint32_t to_read = size;
    if (pos + to_read > in.size) to_read = in.size - pos;

    uint32_t done = 0;
    while (done < to_read) {
        uint32_t lblock = (pos + done) / SWIFTFS2_BLOCK_SIZE;
        uint32_t offset = (pos + done) % SWIFTFS2_BLOCK_SIZE;
        uint32_t pblock = bmap_read(&in, lblock, 0);
        if (!pblock) break;
        uint8_t *data = cache_get(pblock, 0);
        uint32_t chunk = SWIFTFS2_BLOCK_SIZE - offset;
        if (chunk > to_read - done) chunk = to_read - done;
        memcpy((uint8_t *)buf + done, data + offset, chunk);
        done += chunk;
    }

    g_fds[fd].pos = pos + done;
    return done;
}

int swiftfs2_write(int fd, const void *buf, uint32_t size) {
    if (fd < 0 || fd >= SWIFTFS2_MAX_FD || !g_fds[fd].inode) return -1;
    if (g_fds[fd].flags == O_RDONLY) return -1;

    swiftfs2_inode_t in;
    inode_read(g_fds[fd].inode, &in);

    uint32_t pos = g_fds[fd].pos;
    uint32_t done = 0;

    while (done < size) {
        uint32_t lblock = (pos + done) / SWIFTFS2_BLOCK_SIZE;
        uint32_t offset = (pos + done) % SWIFTFS2_BLOCK_SIZE;
        uint32_t pblock = bmap_read(&in, lblock, 1);
        if (!pblock) break;
        uint8_t *data = cache_get(pblock, 0);
        uint32_t chunk = SWIFTFS2_BLOCK_SIZE - offset;
        if (chunk > size - done) chunk = size - done;
        memcpy(data + offset, (const uint8_t *)buf + done, chunk);
        cache_mark_dirty(pblock, 0);
        done += chunk;
    }

    pos += done;
    if (pos > in.size) in.size = pos;
    in.block_count = (in.size + SWIFTFS2_BLOCK_SIZE - 1) / SWIFTFS2_BLOCK_SIZE;
    inode_write(g_fds[fd].inode, &in);

    g_fds[fd].pos = pos;
    return done;
}

int swiftfs2_close(int fd) {
    if (fd < 0 || fd >= SWIFTFS2_MAX_FD || !g_fds[fd].inode) return -1;

    if (g_fds[fd].dirty) {
        swiftfs2_inode_t in;
        inode_read(g_fds[fd].inode, &in);
        inode_write(g_fds[fd].inode, &in);
    }

    memset(&g_fds[fd], 0, sizeof(g_fds[fd]));
    return 0;
}

/* ========== Directory operations ========== */

/* Add an entry to a directory */
static int dir_add_entry(uint32_t dir_ino, const char *name,
                         uint32_t child_ino, uint8_t file_type) {
    swiftfs2_inode_t dir_in;
    inode_read(dir_ino, &dir_in);

    int nlen = strlen(name);
    uint16_t reclen = sizeof(swiftfs2_dirent_t) + nlen;
    reclen = (reclen + 3) & ~3; /* align to 4 */

    /* Scan directory for a free slot or end */
    for (uint32_t bi = 0; ; bi++) {
        uint32_t pblock = bmap_read(&dir_in, bi, 0);
        uint8_t *data;

        if (!pblock) {
            pblock = block_alloc();
            if (!pblock) return -1;
            data = cache_get(pblock, 0);
            memset(data, 0, SWIFTFS2_BLOCK_SIZE);
            cache_mark_dirty(pblock, 0);
            /* Link block to inode */
            if (bi < SWIFTFS2_DIRECT_COUNT)
                dir_in.direct[bi] = pblock;
            else if (bi == SWIFTFS2_DIRECT_COUNT) {
                if (!dir_in.indirect) {
                    dir_in.indirect = block_alloc();
                    if (!dir_in.indirect) return -1;
                    uint8_t *z = cache_get(dir_in.indirect, 0);
                    memset(z, 0, SWIFTFS2_BLOCK_SIZE);
                    cache_mark_dirty(dir_in.indirect, 0);
                }
                uint32_t *tbl = (uint32_t *)cache_get(dir_in.indirect, 0);
                tbl[bi - SWIFTFS2_DIRECT_COUNT] = pblock;
                cache_mark_dirty(dir_in.indirect, 0);
            }
            dir_in.size += SWIFTFS2_BLOCK_SIZE;
            dir_in.block_count++;
            inode_write(dir_ino, &dir_in);

            /* New block: write entry at start with full block as rec_len */
            {
                swiftfs2_dirent_t *de = (swiftfs2_dirent_t *)data;
                de->inode = child_ino;
                de->rec_len = SWIFTFS2_BLOCK_SIZE;
                de->name_len = nlen;
                de->file_type = file_type;
                memcpy(de->name, name, nlen);
                cache_mark_dirty(pblock, 0);
                return 0;
            }
        } else {
            data = cache_get(pblock, 0);
        }

        uint32_t pos = 0;
        uint32_t best_pos = SWIFTFS2_BLOCK_SIZE;
        int found_slot = 0;

        while (pos < SWIFTFS2_BLOCK_SIZE) {
            swiftfs2_dirent_t *de = (swiftfs2_dirent_t *)(data + pos);
            if (de->inode == 0 && de->rec_len >= reclen) {
                best_pos = pos;
                found_slot = 1;
                break;
            }
            if (de->rec_len == 0) {
                /* Corrupt entry, treat as free */
                best_pos = pos;
                found_slot = 1;
                break;
            }
            pos += de->rec_len;
        }

        if (found_slot) {
            swiftfs2_dirent_t *de = (swiftfs2_dirent_t *)(data + best_pos);
            uint16_t remaining = de->rec_len;
            de->inode = child_ino;
            de->name_len = nlen;
            de->file_type = file_type;
            memcpy(de->name, name, nlen);
            if (remaining >= reclen + sizeof(swiftfs2_dirent_t)) {
                swiftfs2_dirent_t *next =
                    (swiftfs2_dirent_t *)(data + best_pos + reclen);
                next->inode = 0;
                next->rec_len = remaining - reclen;
                de->rec_len = reclen;
            } else {
                de->rec_len = remaining;
            }
            cache_mark_dirty(pblock, 0);
            return 0;
        }

        /* No space in this block, try next */
    }
}

int swiftfs2_mkdir(const char *path, uint16_t mode) {
    if (!g_mounted) return -1;

    /* Find parent dir */
    char parent_path[SWIFTFS2_NAME_MAX * 2];
    char dirname[SWIFTFS2_NAME_MAX];
    uint32_t plen = strlen(path);
    if (plen >= SWIFTFS2_NAME_MAX * 2) plen = SWIFTFS2_NAME_MAX * 2 - 1;
    memcpy(parent_path, path, plen);
    parent_path[plen] = 0;

    /* Find last / */
    char *slash = 0;
    for (char *p = parent_path; *p; p++)
        if (*p == '/') slash = p;

    uint32_t parent_ino;
    if (slash) {
        *slash = 0;
        if (path_walk(parent_path, &parent_ino) < 0) return -1;
        memcpy(dirname, slash + 1, SWIFTFS2_NAME_MAX);
    } else {
        parent_ino = 1; /* root */
        memcpy(dirname, path, SWIFTFS2_NAME_MAX);
    }

    /* Allocate inode */
    uint32_t ino = inode_alloc(S_IFDIR | (mode & 0777));
    if (!ino) return -1;

    /* Add . and .. */
    if (dir_add_entry(ino, ".", ino, SWIFTFS2_FILE_TYPE_DIR) < 0
        || dir_add_entry(ino, "..", parent_ino, SWIFTFS2_FILE_TYPE_DIR) < 0) {
        inode_free(ino);
        return -1;
    }

    /* Add entry in parent */
    if (dir_add_entry(parent_ino, dirname, ino, SWIFTFS2_FILE_TYPE_DIR) < 0) {
        inode_free(ino);
        return -1;
    }

    return 0;
}

int swiftfs2_unlink(const char *path) {
    /* Find parent dir and entry name */
    char parent_path[SWIFTFS2_NAME_MAX * 2];
    char name[SWIFTFS2_NAME_MAX];
    uint32_t plen = strlen(path);
    if (plen >= SWIFTFS2_NAME_MAX * 2) plen = SWIFTFS2_NAME_MAX * 2 - 1;
    memcpy(parent_path, path, plen);
    parent_path[plen] = 0;

    char *slash = 0;
    for (char *p = parent_path; *p; p++)
        if (*p == '/') slash = p;

    uint32_t parent_ino;
    if (slash) {
        *slash = 0;
        if (path_walk(parent_path, &parent_ino) < 0) return -1;
        memcpy(name, slash + 1, SWIFTFS2_NAME_MAX);
    } else {
        parent_ino = 1;
        memcpy(name, path, SWIFTFS2_NAME_MAX);
    }

    uint32_t target_ino;
    if (path_walk(path, &target_ino) < 0) return -1;

    swiftfs2_inode_t target_in;
    inode_read(target_ino, &target_in);
    if (target_in.mode & S_IFDIR) {
        /* Check dir is empty (only . and ..) */
        /* simplified: allow unlink on directories */
    }

    /* Remove from parent directory */
    swiftfs2_inode_t parent_in;
    inode_read(parent_ino, &parent_in);
    int nlen = strlen(name);

    for (uint32_t bi = 0; ; bi++) {
        uint32_t pblock = bmap_read(&parent_in, bi, 0);
        if (!pblock) break;
        uint8_t *data = cache_get(pblock, 0);
        uint32_t pos = 0;
        while (pos < SWIFTFS2_BLOCK_SIZE) {
            swiftfs2_dirent_t *de = (swiftfs2_dirent_t *)(data + pos);
            if (de->inode == 0) break;
            if (de->name_len == (uint8_t)nlen
                && strncmp(de->name, name, nlen) == 0) {
                de->inode = 0;
                cache_mark_dirty(pblock, 0);
                goto remove_done;
            }
            pos += de->rec_len;
        }
    }
    return -1; /* not found */

remove_done:
    /* Free blocks and inode */
    swiftfs2_inode_t in;
    inode_read(target_ino, &in);
    for (int i = 0; i < SWIFTFS2_DIRECT_COUNT; i++) {
        if (in.direct[i]) { block_free(in.direct[i]); in.direct[i] = 0; }
    }
    if (in.indirect) { block_free(in.indirect); in.indirect = 0; }
    if (in.double_indirect) { block_free(in.double_indirect); in.double_indirect = 0; }
    inode_free(target_ino);
    return 0;
}

int swiftfs2_ls(const char *path,
                void (*cb)(const char *name, uint16_t mode, uint64_t size)) {
    if (!g_mounted) return -1;

    uint32_t ino;
    if (path_walk(path, &ino) < 0) return -1;

    swiftfs2_inode_t in;
    inode_read(ino, &in);
    if (!(in.mode & S_IFDIR)) return -1;

    for (uint32_t bi = 0; ; bi++) {
        uint32_t pblock = bmap_read(&in, bi, 0);
        if (!pblock) break;
        uint8_t *data = cache_get(pblock, 0);
        uint32_t pos = 0;
        while (pos < SWIFTFS2_BLOCK_SIZE) {
            swiftfs2_dirent_t *de = (swiftfs2_dirent_t *)(data + pos);
            if (de->inode == 0) break;
            if (de->rec_len == 0) break;

            char namebuf[SWIFTFS2_NAME_MAX];
            memcpy(namebuf, de->name, de->name_len);
            namebuf[de->name_len] = 0;

            swiftfs2_inode_t child;
            inode_read(de->inode, &child);

            if (cb) cb(namebuf, child.mode, child.size);

            pos += de->rec_len;
        }
    }
    return 0;
}

/* ========== Mount / sync / umount ========== */

int swiftfs2_mount(int ahci_port) {
    if (g_mounted) return -1;

    g_port = ahci_port;
    memset(g_cache, 0, sizeof(g_cache));
    memset(g_fds, 0, sizeof(g_fds));
    g_cache_next = 0;

    if (read_block(0, &sb) < 0) return -1;
    if (sb.magic != SWIFTFS2_MAGIC) return -1;
    if (sb.block_size != SWIFTFS2_BLOCK_SIZE) return -1;

    if (load_group_descriptors() < 0) return -1;
    if (sb.journal_last < sb.journal_size)
        journal_recover();

    g_curr_group = 0;
    g_mounted = 1;
    serial_printf("[fs] mounted: %u blocks, %u inodes, "
                  "%d groups, %u free\n",
                  (unsigned)sb.num_blocks, (unsigned)sb.inode_count,
                  g_nr_groups, (unsigned)sb.free_blocks);
    return 0;
}

int swiftfs2_sync(void) {
    if (!g_mounted) return -1;

    journal_begin();
    /* Flush inode cache and bitmaps */
    for (int i = 0; i < SWIFTFS2_MAX_FD; i++) {
        if (g_fds[i].inode) {
            g_fds[i].dirty = 0;
        }
    }
    cache_flush_all();
    journal_commit();

    /* Write superblock */
    write_block(0, &sb);

    /* Write group descriptors */
    int desc_blocks = sb.group_desc_count;
    for (int i = 0; i < desc_blocks; i++)
        write_block(1 + i,
                    (uint8_t *)g_gd + i * SWIFTFS2_BLOCK_SIZE);

    /* Sync bitmaps */
    for (int i = 0; i < g_nr_groups; i++)
        bitmap_sync(i);

    return 0;
}

int swiftfs2_umount(void) {
    if (!g_mounted) return -1;
    swiftfs2_sync();

    /* Free allocated memory */
    for (int i = 0; i < g_nr_groups; i++) {
        if (g_bitmaps[i]) kfree(g_bitmaps[i]);
    }
    kfree(g_bitmaps);
    kfree(g_gd);

    memset(g_cache, 0, sizeof(g_cache));
    g_mounted = 0;
    return 0;
}
