# VFS, mmap, brk — SwiftBSD Kernel Design

## 1. VFS (Virtual File System)

### 1.1 Motivation

The kernel currently calls `swiftfs2_*` directly from `syscall_handler.c`.
No abstraction for multiple filesystem types or special devices (/dev/null,
stdin/stdout as character devices).

### 1.2 Architecture

```
syscall_handler.c  →  vfs_open/read/write/close/fstat
                              │
                    ┌─────────┼──────────┐
                    │         │          │
              serial_ops  swiftfs_ops  (future: devfs, tmpfs)
```

### 1.3 struct vfs_ops

```c
struct vfs_ops {
    char name[16];
    int  (*open) (const char *path, int flags, void **priv_out);
    int  (*read) (void *priv, void *buf, uint32_t sz, uint64_t *pos);
    int  (*write)(void *priv, const void *buf, uint32_t sz, uint64_t *pos);
    int  (*close)(void *priv);
    int  (*fstat)(void *priv, struct stat *st);
};
```

### 1.4 struct file

```c
#define VFS_MAX_FILES 64

struct file {
    struct vfs_ops *ops;
    void *priv;         // FS-specific (swiftfs int fd, or NULL)
    uint64_t pos;       // current file position
    int flags;          // O_RDONLY etc.
    int used;
};
```

### 1.5 fd layout

| fd | owner       | ops         |
|----|-------------|-------------|
| 0  | stdin       | serial_ops  |
| 1  | stdout      | serial_ops  |
| 2  | (reserved)  | —           |
| 3+ | files       | swiftfs_ops |

### 1.6 Operations

- `vfs_open(path, flags)` — iterates registered `vfs_ops`, tries each `.open`,
  fills a `struct file`, returns fd
- `vfs_read(fd, buf, sz)` — dispatch `file->ops->read(file->priv, buf, sz, &file->pos)`
- `vfs_write(fd, buf, sz)` — dispatch `file->ops->write(file->priv, buf, sz, &file->pos)`
- `vfs_close(fd)` — dispatch, free slot
- `vfs_fstat(fd, st)` — dispatch `file->ops->fstat(file->priv, st)`

### 1.7 stdin/stdout (serial_ops)

```c
static int serial_open(const char *path, int flags, void **priv) {
    (void)path; (void)flags; (void)priv;
    return 0;  // always succeeds
}

static int serial_read(void *priv, void *buf, uint32_t sz, uint64_t *pos) {
    (void)priv; (void)pos;
    for (uint32_t i = 0; i < sz; i++) ((char*)buf)[i] = serial_getc();
    return sz;
}

static int serial_write(void *priv, const void *buf, uint32_t sz, uint64_t *pos) {
    (void)priv; (void)pos;
    for (uint32_t i = 0; i < sz; i++) serial_putc(((char*)buf)[i]);
    return sz;
}
```

Pre-opened at boot: `vfs_init()` creates `files[0]` and `files[1]` with `serial_ops`.

### 1.8 SwiftFS ops

Wrapper functions in `swiftfs2.c` that implement `vfs_ops`:

```c
static int sw_open(const char *path, int flags, void **priv) {
    int fd = swiftfs2_open(path, flags);
    if (fd < 0) return -1;
    *(int*)priv = fd;
    return 0;
}

static int sw_read(void *priv, void *buf, uint32_t sz, uint64_t *pos) {
    int fd = *(int*)priv;
    int ret = swiftfs2_read(fd, buf, sz);
    if (ret > 0) *pos += ret;
    return ret;
}

// sw_write, sw_close, sw_fstat similarly
```

Registered once at boot via `vfs_register(&swiftfs_ops)`.

---

## 2. brk

### 2.1 Motivation

Userland heap: traditionally `malloc` calls `brk`/`sbrk` to grow the data segment.

The ELF loader sets `heap_start = end of BSS` (highest mapped page).
`brk` moves the break. Page faults on pages between `heap_start` and `heap_end`
map anonymous physical pages.

### 2.2 Data

```c
// In struct process (process.h)
uint64_t heap_break;
```

Set during `exec` and `proc_create`:

```c
// After loading ELF:
proc->heap_break = highest_mapped_page + PAGE_SIZE;
```

### 2.3 SC_BRK (syscall 15)

```c
void *brk(void *addr);
```

- `brk(0)`: return current `heap_break`
- `brk(addr)` where `addr < heap_start` (below last ELF page): return -1 (ENOMEM)
- `brk(addr)` where `addr > heap_break`: page-align, map pages from `heap_break` to `addr`
  with `PG_PRESENT | PG_WRITE | PG_USER | PG_NX`
- `brk(addr)` where `addr < heap_break`: unmap pages, update `heap_break`
- `brk(addr)` where `addr` is in the same page: just update `heap_break`
- Return the new `heap_break`

### 2.4 Page fault interaction

For brk, pages are mapped eagerly on `brk()` call (not demand-paged).
This keeps the page fault handler simple and avoids distinguishing "brk fault"
from "mmap fault" in the handler.

---

## 3. mmap / munmap / msync

### 3.1 VMA (Virtual Memory Area) — per-process

```c
#define VMA_MAX 16

struct vma {
    uint64_t start;       // inclusive
    uint64_t end;         // exclusive
    int prot;             // PROT_READ | PROT_WRITE | PROT_EXEC
    int flags;            // MAP_SHARED / MAP_PRIVATE / MAP_ANONYMOUS
    struct file *file;    // NULL for anonymous
    uint64_t file_off;    // offset in file
};
```

```c
// In struct process (process.h)
struct vma vmas[VMA_MAX];
int vma_count;
```

### 3.2 SC_MMAP (syscall 16)

```c
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
```

- `addr`: hint address (only `NULL` or page-aligned supported; MAP_FIXED not yet)
- `length`: rounded up to PAGE_SIZE
- `prot`: PROT_READ, PROT_WRITE, PROT_EXEC
- `flags`: MAP_PRIVATE, MAP_SHARED, MAP_ANONYMOUS
- `fd`: ignored if MAP_ANONYMOUS; else fd from a previous `open`
- `offset`: file offset (page-aligned)

**Process:**

1. Allocate VA range: scan VMA list for a free gap.
   For now: `addr == NULL` → pick address after existing mappings
   (start searching from 0x10000000 upward).
2. Find a free VMA slot.
3. Fill VMA fields.
4. For **MAP_ANONYMOUS**: allocate physical pages immediately, map them.
   (Simple: no demand paging for anonymous.)
5. For **file-backed**: only reserve VMA. Actual pages allocated on page faults.
6. Return `start` address, or `MAP_FAILED ((void*)-1)` on error.

### 3.3 SC_MUNMAP (syscall 17)

```c
int munmap(void *addr, size_t length);
```

1. Find VMA covering `[addr, addr+length)`.
2. For MAP_SHARED file mappings: for each page, check Dirty bit in PTE.
   If dirty, seek file to `file_off + page_index` and write PAGE_SIZE bytes.
3. Unmap page table entries.
4. Free physical pages.
5. Remove or shrink VMA.

### 3.4 SC_MSYNC (syscall 18)

```c
int msync(void *addr, size_t length, int flags);
```

1. Find VMA covering `[addr, addr+length)`.
2. For MAP_SHARED file mappings: for each page, if PTE has Dirty bit,
   write page back to file.
3. Clear Dirty bit after writeback.

### 3.5 Page fault handler changes

In `idt.c`, the existing page fault handler:

```c
if (pf_present && pf_write && cr2 >= 0x1000000) {
    // existing CoW logic
}
```

**Add** a new check before the existing CoW logic:

```c
if (!pf_present && cr2 >= 0x40000000) {
    // Check if cr2 falls in any VMA
    for (int i = 0; i < current->vma_count; i++) {
        struct vma *v = &current->vmas[i];
        if (cr2 < v->start || cr2 >= v->end) continue;

        if (v->file) {
            // file-backed demand page
            uint64_t page = cr2 & ~0xFFF;
            uint64_t phys = page_alloc();
            uint64_t file_off = v->file_off + (page - v->start);
            // read from file at file_off into phys
            vfs_read_from_file(v->file, phys, file_off);
            // map with appropriate permissions
            vmm_map(page, phys, flags_from_prot(v->prot, v->flags));
            return;
        }
    }
}
```

For MAP_PRIVATE + write fault: same CoW logic as fork, but for the mmap'd page.

### 3.6 Restrictions (first version)

- No `MAP_FIXED` (addr must be NULL)
- No `MAP_GROWSDOWN`
- No `mprotect`
- No shared anonymous (MAP_SHARED | MAP_ANONYMOUS behaves like MAP_PRIVATE)
- Page cache: none (read from disk on every fault)
- No file locking or synchronization

---

## 4. Syscall numbers (updated)

| #  | Name     | Status     |
|----|----------|------------|
| 0  | puts     | existing   |
| 1  | nop      | existing   |
| 2  | yield    | existing   |
| 3  | open     | existing → VFS |
| 4  | read     | existing → VFS |
| 5  | write    | existing → VFS |
| 6  | close    | existing → VFS |
| 7  | exec     | existing   |
| 8  | exit     | existing   |
| 9  | fstat    | existing → VFS |
| 10 | halt     | existing   |
| 11 | meminfo  | existing   |
| 12 | fork     | existing   |
| 13 | wait     | existing   |
| 14 | getpid   | existing   |
| 15 | brk      | **new**    |
| 16 | mmap     | **new**    |
| 17 | munmap   | **new**    |
| 18 | msync    | **new**    |

---

## 5. File changes summary

| File | Change |
|------|--------|
| `src/kernel/fs/vfs.h` | **new** — struct vfs_ops, struct file, vfs_* declarations |
| `src/kernel/fs/vfs.c` | **new** — VFS table, dispatch, init |
| `src/kernel/fs/swiftfs2.c` | add vfs_ops wrapper, register at boot |
| `src/kernel/fs/swiftfs2.h` | add `swiftfs2_get_inode/fd` helpers if needed |
| `src/kernel/sched/process.h` | add `heap_break`, `vmas[VMA_MAX]`, `vma_count` |
| `src/kernel/user/syscall_handler.c` | use VFS instead of direct swiftfs2 calls; add brk/mmap/munmap/msync |
| `src/kernel/user/syscall.h` (userland) | add SC_BRK, SC_MMAP, SC_MUNMAP, SC_MSYNC |
| `src/kernel/arch/x86_64/idt.c` | add VMA-aware page fault handling |
| `src/kernel/elf.c` | return BSS end (highest mapped page) for heap_start |
| `src/kernel/sched/sched.c` | init heap_break, vma_count on proc_create/fork |
| `src/kernel/user/elf.h` | (if needed) expose BSS end |
| `Makefile` | add `build/vfs.o` |
