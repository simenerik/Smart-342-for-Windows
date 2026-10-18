/* ppc.h - 32-bit PowerPC user-mode interpreter (big-endian guest) */
#ifndef PPC_H
#define PPC_H
#include <stdint.h>

#define PPC_CHUNK_BITS 16
#define PPC_CHUNK_SIZE (1u << PPC_CHUNK_BITS)
#define PPC_NCHUNKS (1u << (32 - PPC_CHUNK_BITS))

enum { PPC_STOP_HOOK = 1, PPC_STOP_COUNT = 2, PPC_STOP_FAULT = 3, PPC_STOP_ILLEGAL = 4 };

typedef struct PPC {
    uint32_t gpr[32];
    uint64_t fpr[32];        /* raw IEEE-754 double bits */
    uint32_t cr, xer, lr, ctr, fpscr, pc, msr;
    uint8_t *chunk[PPC_NCHUNKS];   /* host memory per 64 KB guest chunk (NULL = unmapped) */
    /* execution of any address in [hook_lo, hook_hi) stops the run with PPC_STOP_HOOK before executing it */
    uint32_t hook_lo, hook_hi;
    uint64_t icount;
    uint32_t fault_addr; int fault_write; uint32_t illegal_insn;
} PPC;

PPC *ppc_new(void);
void ppc_free(PPC *p);
int ppc_map(PPC *p, uint32_t addr, uint32_t size);          /* allocate zeroed memory, 0 on success */
uint8_t *ppc_host(PPC *p, uint32_t addr);                    /* host pointer or NULL */
int ppc_read(PPC *p, uint32_t addr, void *dst, uint32_t n);  /* 0 ok, -1 unmapped */
int ppc_write(PPC *p, uint32_t addr, const void *src, uint32_t n);
int ppc_run(PPC *p, uint64_t max_insns);                     /* returns PPC_STOP_* */
int ppc_step(PPC *p);                                        /* single instruction */

#endif
