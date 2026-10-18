/* popups.c: the names the original's Setup popups offer (see there) */
#ifndef POPUPS_H
#define POPUPS_H
enum { POP_GRAD, POP_SHARP, POP_COLOR, POP_FILM, POP_MEDIA, POP_ENDP, POP_N };
#define POP_MAX 64
typedef struct { int n, cur; char item[POP_MAX][96]; } PopList;
extern PopList pop_lists[POP_N];
extern volatile int pop_seq;
void popups_install(Emu *E);
int popups_read_all(Emu *E);
int popups_select_filmtype(Emu *E, const char *name_utf8);   /* call from the front end's idle (see popups.c) */
const char *popup_name(int k);
#endif
