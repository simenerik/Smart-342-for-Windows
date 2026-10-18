#include "pef.h"
#include <stdio.h>
int main(int argc, char **argv) {
    char err[256]; Pef *P = pef_load(argv[1], err, sizeof err);
    if (!P) { printf("ERR %s\n", err); return 1; }
    printf("code %u data %u toc %x imports %d exports %d relocs %d funcs %d main %d:%x\n", P->code_len, P->data_len, P->toc, P->nimports, P->nexports, P->nrelocs, P->nfuncs, P->main_sec, P->main_off);
    for (int i = 0; i < P->nfuncs; i++) printf("F %06x %s\n", P->funcs[i].addr, P->funcs[i].name);
    for (int i = 0; i < P->nrelocs; i++) printf("R %x %c %x\n", P->relocs[i].off, P->relocs[i].kind, P->relocs[i].val);
    for (int i = 0; i < P->nimports; i++) printf("I %s %d %d\n", P->imports[i].name, P->imports[i].cls, P->imports[i].lib);
    return 0;
}
