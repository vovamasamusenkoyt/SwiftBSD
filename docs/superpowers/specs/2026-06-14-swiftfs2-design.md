# SwiftFS v2 — Full Read-Write Filesystem

## Overview

Upgrade SwiftFS from a minimal read-only flat directory FS to a full read-write filesystem with inodes, nested directories, block groups, journaling, block cache, and Unix permissions.

Target: 1TB max disk, 4096-byte blocks, QEMU AHCI.

---

## On-Disk Format

### Block size
- 4096 bytes (8 AHCI sectors)
- Aligned to x86-64 page size

### Layout

```
Block group 0:   [superblock | group descriptors | block bitmap | inode table | data blocks]
Block group 1-N: [block bitmap | inode table | data blocks]
```

### Block groups
- 32768 blocks per group = 128MB per group
- 1TB / 128MB = 8192 groups max

### Superblock (block 0 of group 0)

```c
struct swiftfs2_superblock {
    uint64_t magic;            // "SWIFTFS2" (0x3253465446495753)
    uint64_t block_size;       // 4096
    uint64_t num_blocks;       // total blocks on device
    uint64_t inode_count;      // total inodes
    uint64_t free_blocks;      // global free count
    uint64_t free_inodes;
    uint32_t blocks_per_group; // 32768
    uint32_t inodes_per_group;
    uint32_t group_desc_count; // how many group descriptor blocks
    uint32_t inode_size;       // 128
    // journal
    uint64_t journal_block;    // starting block of journal
    uint64_t journal_size;     // in blocks (8192 = 32MB)
    uint64_t journal_last;     // last checkpointed transaction
    uint8_t  uuid[16];
    uint8_t  volume_name[32];
    uint8_t  pad[3768];        // fill to 4096
} __attribute__((packed));
```

### Group descriptor array

Immediately after superblock (blocks 1..group_desc_count).

```c
struct swiftfs2_group_desc {
    uint32_t block_bitmap_block;  // block containing bitmap
    uint32_t inode_table_block;   // first block of inode table
    uint32_t free_blocks_count;
    uint32_t free_inodes_count;
    uint32_t pad[4];
} __attribute__((packed));
```

Descriptors fit in one block per 1024 groups (128 bytes * 1024 = 128KB, but each is 32 bytes, so 32K entries per 4096 block wait — actually 4096/32=128 descriptors per block. For 8192 groups that's 64 descriptor blocks. Fine.)

### Inode (128 bytes)

```c
struct swiftfs2_inode {
    uint16_t mode;         // file type + permissions (S_IFREG|S_IFDIR, rwxrwxrwx)
    uint16_t uid;
    uint16_t gid;
    uint64_t size;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    uint32_t block_count;  // number of blocks used
    uint32_t direct[12];   // direct block pointers
    uint32_t indirect;     // single indirect block
    uint32_t double_indirect; // double indirect block
    uint8_t  pad[20];      // future: ACL, xattr, etc.
} __attribute__((packed));
```

- direct: 12 * 4096 = 48KB
- indirect: 4096/4 = 1024 entries → 4MB
- double indirect: 1024*1024*4096 = 4GB
- Max file: ~4GB

### Directory entry

Stored in data blocks of a directory inode. Variable-length records.

```c
struct swiftfs2_dirent {
    uint32_t inode;
    uint16_t rec_len;      // total length of this record (4-byte aligned)
    uint8_t  name_len;
    uint8_t  file_type;    // 0=unknown, 1=regular, 2=dir
    char     name[];       // name_len bytes (no NUL terminator on disk)
} __attribute__((packed));
```

- rec_len includes padding to align next entry to 4 bytes
- End of block: record with inode=0 spans to end of block
- Special entries: `.` and `..` (inode of self and parent)

---

## Block Allocator

- Per-group bitmap: 1 bit per block, 4096 bytes = 32768 bits = 32768 blocks
- Allocation: scan bitmap for first zero bit (next-fit with current group hint)
- Fallback to subsequent groups if current is full
- Free: clear bit, update group + global free counts
- Batching: reserve up to 8 blocks atomically within a journal transaction

---

## Journal

### Layout
- Circular buffer of 8192 blocks (32MB) starting at `journal_block`
- Journal tracked in superblock: `journal_last` = last checkpointed transaction ID

### Transaction structure
```
[descriptor block | metadata blocks | commit block]
```

- **Descriptor block**: lists all on-disk block numbers participating in this transaction
- **Metadata blocks**: before-images (or after-images, simple) of inode/bitmap/dir blocks being modified
- **Commit block**: transaction ID + checksum

### Lifecycle
1. **Begin**: allocate transaction ID
2. **Reserve**: for each metadata block to modify, add to transaction
3. **Write**: descriptor + metadata blocks to journal sequentially
4. **Commit**: write commit block (marks transaction complete)
5. **Apply**: copy metadata blocks from journal to their real locations
6. **Checkpoint**: advance `journal_last` in superblock

### Recovery
On mount: scan journal from `journal_last+1`. If a complete transaction (descriptor + data + commit) is found, replay it. Incomplete transactions (no commit block) are discarded.

### Data journaling mode
Metadata-only journaling (data=ordered): data blocks are written to disk first, then metadata is journaled. This avoids double-writing file data while keeping metadata consistent.

---

## Block Cache

- Hash table: key = (group, block_no)
- Cache size: 64 blocks (256KB) — covers inode tables and hot directory blocks
- Entry state: clean / dirty
- LRU eviction of clean entries; dirty entries are written via journal before eviction
- On `sync()` or `umount`: write all dirty blocks → journal → apply → checkpoint

---

## Permissions

- Mode stored in inode: 9 bits rwxrwxrwx + file type bits (S_IFREG=0100000, S_IFDIR=0040000)
- uid=0 bypasses all checks (root)
- Non-root: compare caller uid/gid against inode uid/gid, check corresponding mode bits
- Permission check on: open, mkdir, unlink, write
- umask support at file/dir creation

---

## API

```c
// Mount/unmount
int swiftfs2_mount(int ahci_port);

// File operations
int swiftfs2_open(const char *path, int flags);  // flags: O_RDONLY, O_WRONLY, O_CREAT, O_TRUNC
int swiftfs2_read(int fd, void *buf, uint32_t size);
int swiftfs2_write(int fd, const void *buf, uint32_t size);
int swiftfs2_close(int fd);

// Directory operations
int swiftfs2_mkdir(const char *path, uint16_t mode);
int swiftfs2_unlink(const char *path);
int swiftfs2_ls(const char *path, void (*cb)(const char *name, uint16_t mode, uint64_t size));

// System
int swiftfs2_sync(void);
int swiftfs2_umount(void);

// Internal helpers
static int  path_walk(const char *path, uint32_t *inode_out);
static int  inode_read(uint32_t ino, struct swiftfs2_inode *inode);
static int  inode_write(uint32_t ino, struct swiftfs2_inode *inode);
static uint32_t block_alloc(void);
static void block_free(uint32_t block);
```

---

## TODO: mkfs.swiftfs2

A user-space tool `tools/mkfs.swiftfs2.c` that creates a blank SwiftFS v2 image:
- Writes superblock, group descriptors, zeroed bitmaps and inode tables
- Creates root directory inode (`.` and `..`)
- Invoked as: `./tools/mkfs.swiftfs2 disk.img [size_in_mb]`

Default inodes per group: 4096 (128 blocks of inode table per group).

---

## Implementation Plan

1. New files: `src/kernel/fs/swiftfs2.h`, `src/kernel/fs/swiftfs2.c`
2. Block allocator + group descriptor loading
3. Block cache
4. Inode read/write + path_walk
5. Journal subsystem
6. File ops (open/read/write/close)
7. Directory ops (mkdir, unlink, ls)
8. Integrate into kmain: mount, load user program from new FS
9. Update `tools/mkswiftfs` to create SwiftFS v2 images
10. Remove old SwiftFS v1

---

## Notes

- No symlinks, hard links, ACLs, or extended attributes in v1
- No concurrent access protection (single-core kernel, no multi-thread FS access)
- File descriptors: simple array of open file structs (max 64)
- Double-indirect block not yet needed for typical files but struct reserves it
