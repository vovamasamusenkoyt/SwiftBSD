# Pipes, dup, and userland malloc

## Overview

Add Unix pipe(2)/dup(2) syscalls to SwiftBSD and a minimal user-space malloc
implementation so user programs can dynamically allocate memory.

## Per-process FD table

Currently the VFS file descriptor table (`files[]` in vfs.c) is global — every
process shares the same table.  This breaks pipe semantics: after fork, parent
and child must be able to close their copy of a pipe fd independently.

**Change:** each `struct process` gets its own `struct file fds[VFS_MAX_FILES]`.
A module-level pointer `g_fds` in vfs.c points to the current process's table.
The scheduler updates it on context switch.

### struct file changes

```c
struct file {
    struct vfs_ops *ops;
    void           *priv;
    uint64_t        pos;
    int             flags;
    int             refcount;   /* was: used (0=free, >0=open) */
};
```

`refcount == 0` → slot is free.  `refcount > 0` → slot is open.
`vfs_close` decrements; only calls `ops->close` when it hits 0.

### Boot flow

Before any process exists, kmain uses a static `boot_fds[VFS_MAX_FILES]`
table, activated via `vfs_set_fds(boot_fds)`.  When PID 1 is created,
`proc_create` initialises its fd table with just fd 0/1 (stdin/stdout via
`serial_ops`).

### Context switch

`scheduler` (in sched.c) calls `vfs_set_fds(procs[current_idx].fds)` before
switching to the next process.

### Fork

`proc_fork` memcpy's the parent's fd table into the child and increments
every `refcount`.  This gives the child its own copy of each open fd, and
the underlying `ops->close` is called only when the last reference goes
away.

## Pipe (`SC_PIPE=19`)

### Kernel structure

```c
#define PIPE_BUF_SIZE 4096

struct pipe {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t rpos, wpos;          /* absolute byte counters (modulo gives index) */
    int      readers, writers;
};
```

- `rpos`/`wpos` track logical position; modulo `PIPE_BUF_SIZE` gives the
  circular-buffer offset.
- `readers` — number of open read-end fds.
- `writers` — number of open write-end fds.

### pipe_read

```
while buffer is empty:
    if writers == 0 → return 0 (EOF)
    sched_yield()
copy min(requested, available) bytes from circular buffer, handling wrap
rpos += bytes_read
return bytes_read
```

### pipe_write

```
while buffer is full:
    if readers == 0 → return -1 (broken pipe / no reader)
    sched_yield()
copy min(requested, space) bytes into circular buffer, handling wrap
wpos += bytes_written
return bytes_written
```

### pipe_close

```
if priv is read-end:
    pipe->readers--
if priv is write-end:
    pipe->writers--
if readers == 0 && writers == 0 → kfree(pipe)
```

### VFS ops

Two static `struct vfs_ops` instances:
- `pipe_read_ops` — implements `read` and `close` (write returns -1)
- `pipe_write_ops` — implements `write` and `close` (read returns -1)

### vfs_pipe()

```
struct pipe *p = kmalloc(sizeof(*p))
initialise: rpos=wpos=0, readers=1, writers=1
rfd = fd_alloc()   — assign pipe_read_ops, priv=p, refcount=1
wfd = fd_alloc()   — assign pipe_write_ops, priv=p, refcount=1
fds[0]=rfd, fds[1]=wfd
return 0
```

## Dup (`SC_DUP=20`, `SC_DUP2=21`)

- **SC_DUP(oldfd):** find lowest free slot, copy `g_fds[oldfd]` into it,
  increment refcount, return the new fd number.
- **SC_DUP2(oldfd, newfd):** if `g_fds[newfd]` is open, close it first.
  Then copy `g_fds[oldfd]` into slot `newfd`, increment refcount.

Both return the new fd number on success, -1 on error.

## Userland malloc (`src/userland/malloc.h`)

Segregated free-list allocator built on top of `brk()`.

### Data structures

```c
struct malloc_header {
    size_t size;            /* total size including header, 8-byte aligned */
    struct malloc_header *next;
};
```

A free block stores the header at its start; the usable area follows
immediately after.  Allocated blocks have the same layout but are not
linked into the free list.

### malloc(n)

- Round `n` up to 8 bytes, add `sizeof(struct malloc_header)`.
- Search free list for a block with `size >= requested`.
- If found: unlink, split if remainder >= 32 bytes (min block).
- If not found: `sbrk(requested)`, treat as single block.
- Return `(void*)(header + 1)`.

### free(p)

- Get header via `(struct malloc_header *)p - 1`.
- Insert into free list (sorted by address for easy coalescing).
- Coalesce adjacent free blocks.

### realloc(p, n)

- If `p == NULL` → `malloc(n)`.
- If `n == 0` → `free(p)`; return NULL.
- Allocate new block, copy `min(old_size, n)` bytes, free old.

### calloc(nmemb, size)

- `malloc(nmemb * size)`, zero-fill.

## Syscall numbers

```
SC_PIPE   = 19
SC_DUP    = 20    /* dup(oldfd) */
SC_DUP2   = 21    /* dup2(oldfd, newfd) */
```

## Userland wrappers (syscall.h)

```c
static int pipe(int fds[2]) { return syscall(SC_PIPE, (long)fds, 0, 0); }
static int dup(int oldfd) { return syscall(SC_DUP, oldfd, 0, 0); }
static int dup2(int oldfd, int newfd) { return syscall(SC_DUP2, oldfd, newfd, 0); }
```

## Testing strategy

1. Boot kernel — confirm shell starts.
2. Run `echo` / `ls` / `cat` — no regression.
3. Update shell to support `|` operator (future PR).
