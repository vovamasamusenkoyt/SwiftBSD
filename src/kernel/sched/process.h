#pragma once
#include <stdint.h>
#include "vfs.h"
#include "vmm.h"

#define MAX_PROCS 64
#define KSTACK_SIZE 4096
#define PROC_NAME_MAX 32

#define PROC_READY   1
#define PROC_ZOMBIE  2
#define PROC_WAITING 3

#define VMA_MAX 32

struct vma {
    uint64_t    start;
    uint64_t    end;
    int         prot;
    int         flags;
    int         fd;
    uint64_t    foff;
    int         used;
};

struct process {
    int used;
    int pid;
    int state;
    int exit_code;
    int parent;

    uint64_t *frame;
    uint64_t  cr3;
    uint64_t  kstack;

    uint64_t    heap_start;
    uint64_t    heap_break;
    struct vma  vmas[VMA_MAX];
    int         vma_count;
};

extern struct process procs[MAX_PROCS];
extern int current_idx;

int  pid_alloc(void);
void pid_free(int pid);
int  proc_index(int pid);
int  proc_create(uint64_t entry, uint64_t rsp, uint64_t cr3, uint64_t kstack);
int  proc_fork(void);
void proc_exit(int code);
int  proc_wait(int *code_out);
void sched_run(void);
