/* msys.h - a small cooperative 'Mac OS' running the Smart342(L) front end and the ScannerDriver
 * background application together (port of scripts/macsys.py). Each application has its own
 * emulator (address space) and Toolbox world; they share the volume, the virtual clock, the
 * directory IDs and the process table. A process runs until WaitNextEvent/GetNextEvent; AppleEvents
 * are routed between the processes, kAEWaitReply blocks the sender until the reply is complete. */
#ifndef MSYS_H
#define MSYS_H
#include "mac.h"
#include "sim.h"

#define APPDIR "Smart342(L) V2.2.0"
#define FG_NAME "Smart342(L)"
#define BG_NAME "ScannerDriver Smart342(L)"

typedef struct Proc {
    char name[64], tag[8];
    uint32_t psn_hi, psn_lo;
    Emu *E; World *W;
    int alive, started;
    char exit_reason[600];
    char idle_at[96];
    struct Coro *co;
    struct MSys *S;
} Proc;

typedef struct MSys {
    char vol[1024];
    FILE *tf;
    int64_t clock;
    DirTab *dirs;
    ProcEnt procs[8]; int nprocs;         /* shared process table (Finder first) */
    Proc *p[4]; int np;                   /* round-robin order */
    int done, fatal; char fatal_tag[8], fatal_msg[600]; char fatal_bt[16][96]; int nfatal_bt;
    uint64_t max_insns;
    long nyield, last_activity;
    struct Coro *main;
    /* SCSI target factory for the driver process: fills targets[0..7] */
    void (*scsi_target)(struct MSys *S, Proc *bg, ScsiTarget **targets);
    void *scsi_ctx;
    struct Bus *bus;
    /* front-end idle hook (scenario runner / Windows UI) */
    void (*on_fg_idle)(struct MSys *S, Proc *fg);
    void (*fg_setup)(struct MSys *S, Proc *fg);       /* after spawn: breakpoints etc. */
    int (*dialog_policy)(World *W, const DlgDesc *d, int n);
    void (*on_log)(World *W, const char *utf8);
    /* reply of an event the host queued (setd/prev/scan): err and reply parameters (errn/errs) */
    void (*on_ae_done)(struct MSys *S, Proc *p, AERec *req, AERec *reply, int err);
    /* every Apple event sent between the processes (e.g. the driver's FPRV/FSCN 'finished' events) */
    void (*on_route)(struct MSys *S, Proc *from, Proc *to, const AERec *ev);
    void *user;
} MSys;

MSys *msys_new(const char *vol, const char *trace_path, uint64_t max_insns);
/* runs until the front end exits or a fatal error; returns 0 on a clean stop */
int msys_run_fg(MSys *S);
Proc *msys_find(MSys *S, const char *name);

#endif
