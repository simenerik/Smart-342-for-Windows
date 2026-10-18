/* pef.h - PEF container loader (Apple, "Mac OS Runtime Architectures", ch. 8) */
#ifndef PEF_H
#define PEF_H
#include <stdint.h>
#include <stddef.h>

typedef struct { char *name; int cls; int lib; } PefImport;
typedef struct { char *name; int cls; uint32_t val; int sec; } PefExport;
typedef struct { uint32_t off; char kind; uint32_t val; } PefReloc;   /* kind 'C','D','I' */
typedef struct { uint32_t addr; const char *name; } PefFunc;

typedef struct Pef {
    uint8_t *code; uint32_t code_len;
    uint8_t *data; uint32_t data_len;     /* unrelocated data image (pidata unpacked) */
    int code_idx, data_idx;
    int32_t main_sec; uint32_t main_off;
    PefImport *imports; int nimports;
    char **libs; int nlibs;
    PefExport *exports; int nexports;
    PefReloc *relocs; int nrelocs;         /* sorted by off */
    uint32_t toc;                         /* data offset of TOC */
    PefFunc *funcs; int nfuncs;            /* sorted by addr: code offset -> name */
} Pef;

Pef *pef_load(const char *path, char *err, size_t errlen);
void pef_free(Pef *p);
const PefReloc *pef_reloc_at(const Pef *p, uint32_t off);
/* name of the function containing code offset a ("?" if none); *start receives its start */
const char *pef_owner(const Pef *p, uint32_t a, uint32_t *start);
int pef_func_addr(const Pef *p, const char *name, uint32_t *addr);
#endif
