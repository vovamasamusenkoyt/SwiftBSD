# Anonymous demand paging, VMA split/merge, mprotect

## 1. Demand paging for anonymous mmap

Currently `SC_MMAP` for anonymous mappings allocates all pages immediately
in the syscall handler. Change: only register a VMA, allocate pages on
demand in the page-fault handler (same as file-backed MAP_SHARED).

### 1.1 mmap changes

Remove the page-allocation loop from the anonymous path in `syscall_handler.c`.
Both anonymous and file-backed paths become:

```
find free VMA slot
pick address (hint @ 0x40000000+)
fill VMA: start, end, prot, flags, fd, foff
mark used
```

- `fd >= 0` → file-backed (existing, unchanged)
- `fd < 0` → anonymous (new)

### 1.2 Page-fault handler changes (idt.c)

Current flow for non-present PF:
1. If `cr2 >= 0x1000000` and not-present: check file-backed VMA → read from file.
2. If no VMA matched: PF panic.

Add anonymous VMA handling in the same path:

```c
for each VMA:
  if v->fd >= 0:
    // file-backed (existing)
    vmm_alloc_page, vmm_map, read from file
    return
  if v->fd < 0:
    // anonymous (new)
    phys = vmm_alloc_page()  // already zeroed
    vmm_map(page, phys, PTE flags from v->prot)
    return
```

## 2. VMA split and partial munmap

### 2.1 Helper: vma_split

```c
// Split VMA at idx at split_addr.
// Left part: [start, split_addr), right part: [split_addr, end)
// Returns index of right part, or -1.
static int vma_split(struct vma *vmas, int idx, uint64_t split_addr);
```

Implementation:

```c
int right = find_free_vma();
vmas[right] = vmas[idx];           // copy flags/fd/foff
vmas[right].start = split_addr;    // right part
// vmas[right].end unchanged

vmas[idx].end = split_addr;        // left part truncated

// fix foff for right part
vmas[right].foff += split_addr - vmas[idx].start;

return right;
```

### 2.2 Partial munmap

Current munmap marks whole VMA unused even when `[addr, addr+len)` covers
only part of it. New logic in `SC_MUNMAP`:

```
for each VMA that overlaps [addr, addr+len):
    // 1. Trim start
    if (addr > v->start) {
        vma_split(vmas, i, addr);
        // now v->end == addr, right part starts at addr
        i = right;  // continue with right part
    }
    // 2. Trim end
    if (addr + len < v->end) {
        vma_split(vmas, i, addr + len);
        // now v->end == addr+len, right part starts at addr+len
    }
    // 3. Now VMA exactly covers [addr, addr+len)
    //    (or was split into exact pieces)
    for each page in [addr, addr+len):
        if MAP_SHARED dirty: writeback
        vmm_unmap(page)
    mark VMA unused
    try_merge adjacent VMAs
```

### 2.3 Helper: vma_try_merge

```c
static void vma_try_merge(struct vma *vmas, int idx) {
    // check idx-1: if vmas[idx-1].end == vmas[idx].start
    //   and same prot/flags/fd, merge
    // check idx+1: symmetric
}
```

Merge condition: `end == start && prot == prot && flags == flags && fd == fd &&
(fd < 0 || foff1 + (end1-start1) == foff2)`.

## 3. mprotect

### 3.1 SC_MPROTECT (syscall 22)

```
void *mprotect(void *addr, size_t len, int prot);
```

Semantics:

1. Find VMA covering `addr`.
2. If `addr` is not at a VMA boundary → split at `addr`.
3. If `addr + len` is not at a VMA boundary → split at `addr + len`.
4. For each fully-enclosed VMA:
   a. Update `v->prot = prot`.
   b. Walk PTEs in `[v->start, v->end)`; for each present PTE:
      - set/clear `PG_WRITE` based on `prot & 2`.
      - set/clear `PG_NX` based on `prot & 4` (NX if not PROT_EXEC).
5. Call `vma_try_merge` on adjacent VMAs.

### 3.2 Error handling

- Return -1 if no VMA covers `addr`.
- Return -1 if the range crosses non-VMA gaps (treat as invalid).

## 4. Limits

- `VMA_MAX` increased from 32 to 64 (split can consume slots).
- VMA merge frees slots when merging two into one.

## 5. Files changed

- `src/kernel/sched/process.h` — VMA_MAX 32→64.
- `src/kernel/user/syscall_handler.c` — SC_MPROTECT=22,
  anonymous mmap no alloc, partial munmap, vma_split/vma_try_merge helpers.
- `src/kernel/arch/x86_64/idt.c` — anonymous demand paging.
- `src/userland/syscall.h` — SC_MPROTECT, mprotect() wrapper.
