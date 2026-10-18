/* mac.h - Smart342 runtime: emulator core, Mac Toolbox world, shared helpers.
 * C port of the verified Python reference in scripts/ (emu.py, mac_hle.py, mac_files.py,
 * mac_ui.py, mac_scsi.py, macsys.py). Behaviour and trace formats follow the reference. */
#ifndef MAC_H
#define MAC_H
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <setjmp.h>
#include "ppc.h"
#include "pef.h"

#if defined(__MINGW32__)
#define PRINTF_LIKE(a, b) __attribute__((format(gnu_printf, a, b)))
#else
#define PRINTF_LIKE(a, b) __attribute__((format(printf, a, b)))
#endif

/* ---------------------------------------------------------------- layout */
#define CODE      0x01000000u
#define DATA      0x02000000u
#define IMPTV     0x03000000u
#define IMPSTUB   0x03100000u
#define TRAMP     0x03200000u
#define SENTINEL  0x03300000u
#define LOWMEM    0x00000000u
#define HEAPBASE  0x10000000u
#define HEAPEND   0x38000000u
#define MPBLK     0x0F000000u
#define STACK_TOP 0x40000000u
#define STACK_SIZE 0x00400000u
#define AUX_MP    0x0B000000u
#define AUX_HEAP  0x0C000000u
#define AUX_HEAP_END 0x0F000000u
#define BPZONE    0x01C00000u
#define BLR       0x4E800020u
#define noErr 0
#define fnfErr (-43)
#define dupFNErr (-48)
#define eofErr (-39)
#define posErr (-40)
#define paramErr (-50)
#define dirNFErr (-120)
#define NORET     INT64_MIN          /* HLE function result: leave r3 untouched */

/* --------------------------------------------------------------- buffers */
typedef struct { uint8_t *b; size_t n, cap; } Bytes;
void bytes_put(Bytes *x, const void *s, size_t n);
void bytes_set(Bytes *x, const void *s, size_t n);
void bytes_free(Bytes *x);
Bytes bytes_dup(const void *s, size_t n);

/* ------------------------------------------------------------------ heap */
typedef struct { uint32_t a, n; } Span;
typedef struct {
    uint32_t base, end;
    Span *fr; int nfr, capfr;          /* free list sorted by address */
    uint32_t *ukey, *uval; uint32_t ucap, ucount;   /* used: addr -> size (open addressing) */
} Heap;

typedef struct { uint32_t blk, n; int used; } HandleRec;

struct World;
struct Emu;
typedef int64_t (*HleFn)(struct Emu *E);
typedef void (*BpFn)(struct Emu *E, void *ctx);

typedef struct Emu {
    Pef *P;
    PPC *cpu;
    struct World *W;
    FILE *trace;                 /* may be NULL */
    const char *tag;             /* process tag for multi-process traces ("FG"/"BG") */
    uint32_t toc;
    Heap heap, aux;
    uint32_t mp_next, *mp_free; int nmp_free, capmp_free;
    uint32_t aux_mp_next, *aux_mp_free; int naux_free, capaux_free;
    HandleRec *hmain, *haux;     /* indexed by (mp-MPBLK)/4 and (mp-AUX_MP)/4 */
    uint32_t hmain_cap, haux_cap;
    HleFn *imp_fn;               /* per import index */
    uint64_t *imp_count;
    uint32_t saved_lr, saved_r2;
    int stop; char stopped_reason[512];
    uint32_t bp_next; struct { uint32_t slot; BpFn fn; void *ctx; } *bps; int nbps;
    uint32_t nest_ret;           /* return address for nested calls (hooked) */
    int nest_depth;
    double nest_f1;                   /* f1 after the last emu_call_nested (functions returning a float) */
    int own_nest;                     /* depth of our own calls into the program (emu_call_own), not the HLE's */
    jmp_buf *fail_jmp;           /* where emu_fail() jumps */
    int trace_all;
    const char *cur_hle;         /* HLE function running (python: errors become 'HLE x raised EmuError(...)') */
} Emu;

/* emulator API */
Emu *emu_new(const char *pef_path, FILE *trace, const char *tag);
void emu_reg(const char *name, HleFn fn);            /* global HLE registry */
void emu_bind_imports(Emu *E);                       /* resolve import index -> HleFn */
uint32_t emu_r(Emu *E, int n);
void emu_setr(Emu *E, int n, uint32_t v);
uint32_t A(Emu *E, int i);
int32_t SA(Emu *E, int i);                           /* low 16 bits, signed */
int32_t SA32(Emu *E, int i);
double FA(Emu *E, int i);
void SETF1(Emu *E, double x);
void emu_read(Emu *E, uint32_t a, void *dst, uint32_t n);
void emu_write(Emu *E, uint32_t a, const void *src, uint32_t n);
void emu_zero(Emu *E, uint32_t a, uint32_t n);
uint8_t rd8(Emu *E, uint32_t a); uint16_t rd16(Emu *E, uint32_t a); int16_t rds16(Emu *E, uint32_t a);
uint32_t rd32(Emu *E, uint32_t a);
void wr8(Emu *E, uint32_t a, uint32_t v); void wr16(Emu *E, uint32_t a, uint32_t v); void wr32(Emu *E, uint32_t a, uint32_t v);
int rd_pstr(Emu *E, uint32_t a, char *out);          /* raw Mac Roman, NUL terminated; returns length */
int rd_cstr(Emu *E, uint32_t a, char *out, int maxn);
void wr_pstr(Emu *E, uint32_t a, const char *s, int n);   /* n<0: strlen */
void wr_cstr(Emu *E, uint32_t a, const char *s);
uint32_t new_ptr(Emu *E, uint32_t n, int aux);
void dispose_ptr(Emu *E, uint32_t a);
int64_t ptr_size(Emu *E, uint32_t a);                /* -1 if not a block */
uint32_t new_handle(Emu *E, uint32_t n, const void *data, int aux);
HandleRec *handle_rec(Emu *E, uint32_t h);           /* NULL if not a live handle */
uint32_t handle_size(Emu *E, uint32_t h);
uint8_t *handle_copy(Emu *E, uint32_t h, uint32_t *n);   /* malloc'd copy of the data */
void set_handle_size(Emu *E, uint32_t h, uint32_t n);
void dispose_handle(Emu *E, uint32_t h);
uint32_t new_empty_handle(Emu *E);
void handle_set_block(Emu *E, uint32_t h, uint32_t blk, uint32_t n);
int heap_owns(Emu *E, uint32_t a);
uint32_t emu_call(Emu *E, uint32_t code, uint32_t toc, const uint32_t *args, int nargs, uint64_t max_insns);
uint32_t emu_call_nested(Emu *E, uint32_t tvec, const uint32_t *args, int nargs);
uint32_t emu_call_own(Emu *E, uint32_t tvec, const uint32_t *args, int nargs);
uint32_t emu_entry(Emu *E);                           /* PEF main TVector -> code address */
uint32_t emu_addr_of(Emu *E, const char *name);       /* CODE+offset or 0 */
uint32_t emu_tvec_of(Emu *E, const char *name);       /* exported TVector in data, or 0 */
void emu_add_bp(Emu *E, uint32_t code_off, BpFn fn, void *ctx);
/* fghooks.c: scan-time resolution/scale for the front end (see there) */
void fghooks_install(Emu *E);
void fghooks_set(double res_pxmm, double scale_pct);
void fghooks_set_frame(int frame);
void fghooks_install_frames(Emu *E);
int fghooks_num_frames(Emu *E);
void fghooks_start_preview(Emu *E, int frame);
/* rawscan.c */
double raw_getscale(Emu *E);
void raw_dump(Emu *E, const char *tag);
int raw_maxdet(Emu *E, double cx, double cy, int wpx, int hpx);
int raw_patch_driver(Emu *bg);
int raw_start(Emu *E, int *wpx, int *hpx);
int raw_op_is_maxdet(Emu *E);   /* the driver (BG) is doing a Max Detail now (current_OperationMode 3) */
extern volatile int raw_inpol14;
extern double raw_tab_slope, raw_tab_off;
extern volatile double raw_hdr_k, raw_hdr_t0, raw_hdr_t1, raw_hdr_tmax, raw_last_speed;
extern volatile int raw_hdr_armed, raw_hdr_fired, raw_last_range, raw_hdr_dark;
int raw_hdr_install(Emu *bg);
extern volatile int raw_lim_w, raw_lim_h, raw_lim_w2, raw_lim_h2; extern volatile long raw_lim_area;
extern volatile double raw_res_w;   /* rawscan.c: across resolution of the next RAW (px/mm, 0 = as along) - pixel shift */
int lens_install(Emu *bg);
int focus_install(Emu *bg);
int noalign_install(Emu *bg);
extern volatile int raw_noalign, raw_noalign_fired, raw_lutx_seq, raw_lutx_equal, raw_lutx_n, raw_lutx_g8k;
int stephold_install(Emu *bg);
extern volatile int raw_stephold, raw_stephold_fired, raw_motor_seq, raw_motor_mode, raw_motor_rows, raw_motor_expose;
extern volatile int lens_shift_pulses, lens_shift_approach, lens_shift_slow, lens_shift_slow_used; extern volatile double lens_shift_last_mm, lens_shift_factor;
extern volatile double focus_off_l[5], focus_last_pos, focus_last_nom; extern volatile int focus_seq;
extern volatile int lens_last, lens_seq; extern volatile double lens_mag, lens_pitch;
void emu_fail(Emu *E, const char *fmt, ...) __attribute__((noreturn)) PRINTF_LIKE(2, 3);
const char *emu_where(Emu *E, char *buf);            /* "%06x name" for pc */
const char *emu_where_caller(Emu *E, char *buf);     /* for saved_lr */
int emu_backtrace(Emu *E, char out[][96], int depth);
void T(Emu *E, const char *name, const char *fmt, ...) PRINTF_LIKE(3, 4);
void TLOG(Emu *E, const char *fmt, ...) PRINTF_LIKE(2, 3);   /* raw line to trace */

/* --------------------------------------------------------- Mac Roman text */
int macroman_to_utf8(const char *s, int n, char *out, int outmax);
int utf8_to_macroman(const char *s, char *out, int outmax);
uint32_t macroman_cp(uint8_t c);                     /* unicode code point */
/* Python-compatible repr() of a Mac Roman string decoded to unicode */
void py_repr_str(const char *s, int n, char *out, int outmax);
void py_repr_bytes(const uint8_t *s, int n, char *out, int outmax);
void hexs(const uint8_t *s, int n, char *out);
uint32_t ostype_i(const char *s);
void ostype_s(uint32_t v, char *out);                /* 4 chars + NUL (raw) */

/* -------------------------------------------------------- resource files */
typedef struct { uint32_t type; int16_t id; char *name; uint8_t attr; Bytes data; } Res;
typedef struct ResFile {
    int refnum; char *path; int writable, dirty;
    Res *r; int n, cap;
} ResFile;
ResFile *resfile_new(int refnum, const char *path, const uint8_t *data, size_t len, int writable);
Res *resfile_get(ResFile *rf, uint32_t type, int id);
Res *resfile_put(ResFile *rf, uint32_t type, int id, const char *name, uint8_t attr, const void *d, size_t n);
void resfile_remove(ResFile *rf, uint32_t type, int id);
Bytes resfile_serialize(ResFile *rf);
int rsrc_parse(const uint8_t *buf, size_t len, ResFile *into);

/* ------------------------------------------------------------- AppleEvents */
typedef struct AERec AERec;
typedef struct { char key[5]; char type[5]; Bytes data; AERec *rec; } AEItem;
typedef struct { int done; AERec *reply; } AESlot;
struct AERec {
    int is_list, is_event, is_objspec;
    char cls[5], id[5];
    AEItem *it; int n, cap;           /* params (records/events) or elements (lists) */
    AEItem *attr; int na, capa;
    AESlot *slot;                      /* reply slot of a routed event */
};
AERec *aerec_new(void);
AERec *aerec_copy(const AERec *r);
void aerec_free(AERec *r);
AEItem *aerec_find(AERec *r, const char *key);
AEItem *aerec_put(AERec *r, const char *key, const char *type, const void *d, size_t n, AERec *rec);
int aerec_del(AERec *r, const char *key);
AEItem *aerec_attr(AERec *r, const char *key);
void aerec_set_attr(AERec *r, const char *key, const char *type, const void *d, size_t n);

/* ----------------------------------------------------------------- world */
typedef struct { int ref; char *rel; int rsrc; Bytes data; uint32_t pos; char *path; int dirty; } FCB;
typedef struct { uint32_t psn_hi, psn_lo; char name[64]; char type[5], sig[5]; uint32_t lhi, llo; } ProcEnt;
typedef struct { char cls[5], id[5]; AERec *params; } AEQItem;

/* ModalDialog item description: kind "button", "control", "text" or "edit"; text in Mac Roman */
typedef struct DlgDesc { int k; const char *kind; char text[2048]; int ntext; } DlgDesc;

extern int world_persist_dirids;   /* 1: keep directory IDs in <volume>/.dirids.txt (Windows program) */
typedef struct DirTab { char **path; int n, cap; int ctr; struct { char *rel; int id; } *byp; int nbyp, capbyp; char persist[1100]; } DirTab;

typedef struct World {
    Emu *E;
    char root[4096];
    int vref; char volname[32];
    DirTab *dirs;                     /* shared between processes */
    char *app_dir_rel; int app_dir_id;
    int wd_dir;                       /* 0 = none */
    FCB *fcb; int nfcb, capfcb; int next_ref;
    ResFile **rf; int nrf, caprf;     /* chain, most recent first */
    ResFile *cur_res, *app_res, *sys_res;
    int res_err, res_load, mem_err;
    int64_t *clock;                   /* shared virtual 60 Hz clock */
    int64_t own_clock;
    uint32_t zone;
    FILE *log;
    /* resources -> handles */
    struct { int ref; uint32_t type; int id; uint32_t h; } *rh; int nrh, caprh;
    struct { uint32_t h; int ref; uint32_t type; int id; } *orig; int norig, caporig;
    /* finfo */
    struct { char *rel; char type[5], cre[5]; int flags; } *fi; int nfi, capfi;
    char finfo_path[1100];
    /* processes */
    uint32_t psn_hi, psn_lo;          /* self */
    ProcEnt *procs; int *nprocs;      /* shared table */
    ProcEnt own_procs[8]; int own_nprocs;
    struct { uint32_t hi, lo, mode; } pmode[4]; int npmode;
    /* events / AE */
    AEQItem *q; int nq, capq;
    struct { char cls[5], id[5]; uint32_t h, refcon; } *aeh; int naeh, capaeh;
    AERec *cur_ae;
    struct { uint32_t h; AERec *r; } *descs; int ndescs, capdescs;
    struct { char want[5], cont[5]; uint32_t proc, refcon; } *osl; int nosl, caposl;
    struct { uint32_t p, info; } *m68k; int nm68k, capm68k;
    uint32_t gestalt_key[32], gestalt_val[32]; int ngestalt;
    /* hooks (multi-process system, scenarios) */
    void (*yield_hook)(struct World *W);
    void (*on_idle)(struct World *W);
    int64_t (*launch_hook)(struct World *W, const char *rel, uint32_t *hi, uint32_t *lo);
    /* AESend routing: returns 0 + *reply or an OSErr (<0) */
    int (*route)(struct World *W, AERec *ev, uint32_t mode, AERec **reply);
    void (*on_reply)(struct World *W, AERec *req, AERec *reply, int err);
    void (*on_ae_sent)(struct World *W, AERec *ev);   /* single-process scenarios */
    AERec *(*ae_reply_for)(struct World *W, AERec *ev);
    void *user;                       /* owner (process) */
    void (*on_log)(struct World *W, const char *utf8);   /* scan log messages (SLMS events), UTF-8 */
    int ui_answers[64]; int nui_answers;
    int (*dialog_policy)(struct World *W, const struct DlgDesc *d, int n);   /* NULL: default (OK/Yes/Continue, else first button) */
    struct UI *ui;                    /* mac_ui state */
    struct Bus *bus;                  /* SCSI bus (driver process only) */
    int log_lines;
} World;

World *world_new(Emu *E, const char *root, const char *app_rsrc_path, const char *app_dir_rel, FILE *log, DirTab *shared_dirs);
int dir_id_for(World *W, const char *rel);
const char *dir_rel(World *W, int id);               /* NULL if unknown */
void world_resolve(World *W, int vref, uint32_t dirid, const char *name_mac, char *out, int outmax);
void host_path(World *W, const char *rel, char *out, int outmax);
void host_name_to_rel(const char *host_name, char *out, int outmax);
extern int world_escape_names;
void parent_of(const char *rel, char *out);
void leaf_of(World *W, const char *rel, char *out_mac);   /* Mac Roman leaf name */
void read_fsspec(World *W, uint32_t a, int *v, uint32_t *d, char *name);
void write_fsspec(World *W, uint32_t a, int vref, uint32_t parid, const char *name_mac);
void spec_rel(World *W, uint32_t a, char *out, int outmax);
void get_finfo(World *W, const char *rel, char *type, char *cre, int *flags);
void set_finfo(World *W, const char *rel, const char *type, const char *cre, int flags);
int open_fork(World *W, const char *rel, int rsrc, int *ref);
int close_fork(World *W, int ref);
FCB *fcb_get(World *W, int ref);
int open_res_file(World *W, const char *rel);
void create_res_file(World *W, const char *rel);
ResFile *res_by_ref(World *W, int ref);
void update_res_file(World *W, ResFile *rf);
int64_t ticks(World *W);
void tick(World *W, int64_t n);
void world_delay(World *W, int64_t n);      /* Delay(): waits in hardware mode */
void host_sleep_ms(int ms);
extern int world_realtime;
extern int world_extclock;          /* replay: clock driven by recorded SCSI timing */
void world_set_ext_clock(World *W, int64_t t);
uint32_t now_secs(World *W);
uint32_t get_res(World *W, uint32_t type, int id, int one, const char *name);
int file_exists(World *W, const char *rel);
int is_dir(World *W, const char *rel);
void repr_rel(const char *rel_utf8, char *out, int outmax);   /* python repr() of a host-relative path */
void hle_ae_register(void);

int hle_res_handle_at(World *w, int i, uint32_t *h, int *ref, uint32_t *type, int *id);
int hle_res_handle_info(World *w, uint32_t h, int *ref, uint32_t *type, int *id);
int hle_handle_is_resource(World *w, uint32_t h);
int world_orig3(World *W, uint32_t h, int *ref, uint32_t *type, int *id);
int64_t hle_call_universal_default(Emu *E);

/* HLE module installers */
void hle_uistub_register(void);
void hle_core_register(void);
void hle_files_register(void);
void hle_ui_register(void);
void hle_scsi_register(void);
struct ScsiTarget;
struct Bus *bus_new(World *W, struct ScsiTarget **targets /* [8] by id */, int log);
int bus_ncmd(struct Bus *b);
void world_install_ui(World *W);

/* AE helpers shared by modules */
uint32_t ae_new_desc(World *W, uint32_t addr, AERec *r, const char *type);
AERec *ae_desc_rec(World *W, uint32_t addr);
int ae_dispatch(World *W, AEQItem *item);            /* calls the installed handler; returns OSErr */
void ae_queue_push(World *W, const char *cls, const char *id, AERec *params);

#endif
