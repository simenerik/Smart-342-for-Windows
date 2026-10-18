/* cassette.h - choosing the film holder (cassette format) in the original front end as a user does (cassette.c) */
#ifndef CASSETTE_H
#define CASSETTE_H
#include "mac.h"
/* The front end's cassette menus for this scanner (Smart 342L, CT modes), verified in emulation (docs/cassette-research):
   type = index in the cassette directory STR# 1219 ("342 cassette directory 2.10.96", 0-based; the front end and the
   driver both replace the resource's type_id by the index, InitLocalCassetteDir 0x067f8c / driver 0x07515c) = CATP. */
typedef struct { int type, transparent; const char *name; } CasHolder;
extern const CasHolder CAS_HOLDERS[];
extern const int CAS_NHOLDERS;
const CasHolder *cas_holder(int type);                  /* NULL if the type is not in CAS_HOLDERS */
int cas_legacy_type(const char *code);                  /* 0.1.x 'pcas' code -> the type it really selected; -1 unknown */
int cassette_code_ok(Emu *E);                           /* the front-end code this relies on is the expected 2.2.0 code */
/* Select directory type `type` in the front end's cassette popup ('0006') as a user does (TPopup SetCurrentItem, then
   TSetUp::DoCassettePopItem). Call only from the front end's top-level idle. Returns 1 when the front end's cassette
   (USER_PARAM +0) is `type` afterwards, 0 if its current menu does not offer that type, -1 if it could not be done.
   msg gets a log line. 2 = selected after the original's Restart (a preview existed: its previews, crops and temporary
   files are gone, the driver got 'RSTR'). */
int cassette_select(Emu *E, int type, char *msg, int nmsg);
int cassette_prescan_flag(Emu *E);                     /* the front end's prescan_flag (1 once a preview is the current picture) */
/* Before another MOUNT is sent ('pstp'): the front end ignores a mount change while a preview exists (DoMountPopItem
   0x08532c), so the original's Restart is made first when the mount really changes. transparent: 1 / 0. Returns 1
   nothing needed, 2 restarted, -1 not possible or cancelled (msg says why). */
int cassette_prepare_mount(Emu *E, int transparent, char *msg, int nmsg);
int cassette_current(Emu *E);                           /* the front end's current cassette type (USER_PARAM +0) */
#endif
