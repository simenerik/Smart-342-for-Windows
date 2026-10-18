/* sim.h - behavioural simulator of a Scitex Smart 342(L) over SCSI (port of scripts/sim1.py + sim2.py)
 * and the generic SCSI target interface used by the SCSI Manager HLE. */
#ifndef SIM_H
#define SIM_H
#include "mac.h"

typedef struct ScsiTarget {
    /* returns SCSI status; *din (malloc'd, may be NULL) and *nin receive the data-in phase */
    int (*execute)(struct ScsiTarget *t, const uint8_t *cdb, int ncdb, const uint8_t *out, size_t nout, int has_out,
                   uint32_t alloc, uint8_t **din, size_t *nin);
    void (*destroy)(struct ScsiTarget *t);
    Emu *log;          /* trace sink (TLOG); NULL = silent */
    World *world;
    void (*note)(const char *msg);   /* important events for the user (hardware safety interlock) */
    int has_dt; double last_dt;      /* replay: recorded seconds since the previous command (drives the virtual clock) */
} ScsiTarget;

/* level 1 = sim1, level 2 = sim2. flash: name -> file contents (already wrapped by make_flash_file) */
ScsiTarget *sim_new(int level, Emu *log, const char *flash_name, const uint8_t *flash, size_t nflash);
/* answers from a recorded hardware session (scsi-trace.bin) */
ScsiTarget *replay_open(const char *path, Emu *log);
void replay_stats(ScsiTarget *t, char *msg, int max);
int sim_load_profile(ScsiTarget *t, const char *path);   /* real-scanner reply profile */
uint8_t *make_flash_file(const uint8_t *rsrc, size_t n, size_t *outn);
/* the real scanner through Windows SPTI (NULL + message when not found / not Windows) */
ScsiTarget *spti_open(int id, Emu *log, const char *trace_path, char *msg, int msgmax);
/* 0.3.0: the SCSI devices Windows knows (and, with probe, the ones answering an INQUIRY at the other IDs), LUN 0 */
typedef struct { int port, path, target, initiator, claimed, listed, scitex, type; char vendor[9], product[17], rev[5]; } SptiDev;
int spti_scan(SptiDev *out, int max, int probe);
ScsiTarget *spti_open_at(int port, int path, int id, Emu *log, const char *trace_path, char *msg, int msgmax);
/* 0.5.0: the SCSI card's limits for one pass-through command (IOCTL_SCSI_GET_CAPABILITIES of the scanner's port) */
typedef struct { int known; uint32_t max_transfer, max_pages, align_mask, max_bytes; } SptiCaps;
extern SptiCaps spti_caps;
#define SPTI_NEED_BYTES 307200u   /* the Scitex driver's largest read (SCSI_max_buf_for_scan 0x07e228); hardware: 307,152 seen */
/* bytes one command may carry with a page-aligned buffer: the byte limit and the page limit (0 = none). One page is kept
   as margin ((pages - 1) x 4 KB; review: some port drivers / DMA setups allow one page less than they report) */
static inline uint32_t spti_caps_limit(uint32_t max_transfer, uint32_t max_pages) {
    uint64_t a = max_transfer ? max_transfer : 0xFFFFFFFFu, b = max_pages ? (uint64_t)(max_pages > 1 ? max_pages - 1 : 1) * 4096u : 0xFFFFFFFFu;
    uint64_t m = a < b ? a : b; return m > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)m;
}
extern const char *spti_fake;
/* 0.3.0 (hle_scsi.c): the scanner's parameter file as the scanner software last read it (NULL until then) */
extern uint8_t *scsi_flash_cap; extern size_t scsi_nflash_cap;
/* Safety interlock around a target: commands that would change the scanner's stored parameters are not sent
 * (motion-board NV-RAM writes, flash-disk SCP packets other than open-for-read); they are answered GOOD and logged. */
ScsiTarget *guard_new(ScsiTarget *inner);
/* streamed RAW (guard.c): arm before a RAW; the guard clears it when it sends the Max Detail block as cmd 4 */
extern volatile int raw_stream_armed, raw_stream_sent, sim_fw_dead;
/* 0.4.0 (guard.c): 1 from an image-processor picture block (0x80: preview, RAW, scan) until its end handshake (the first
   write to 0xAE0038 after it; hardware s38/s39: reads, 13 x 0xAE0020, then 0xAE0038) - the window's Stop waits for it */
extern volatile int guard_capture;
/* 0.4.0 test option SIM_EMPTY (sim.c): the driver's DROP_PIXELS / CUT_PIX / INPOL_WIDTH_STEP of the picture being made
   (app.c sets them from the driver's print) */
extern volatile int sim_dsp_drop, sim_dsp_cut, sim_dsp_wcomp; extern volatile double sim_dsp_inpol;
/* 0.2.0: the scanner no longer answers (spti.c: switched off / disconnected; sim.c: --sim-off developer option).
   guard.c stops the scanner software after GONE_LIMIT commands to a scanner that no longer answers (the driver's
   error handling would otherwise recurse until the emulated stack runs out - hardware s36). */
extern volatile int scsi_gone, sim_unplug, guard_gone_cmds;
#define GONE_LIMIT 200
#define GONE_TAG "SCANNER_GONE"
extern volatile long raw_stream_wait, raw_stream_cnt1; extern volatile int raw_stream_cnt_seq;   /* guard.c: the controller's wait counter after a streamed RAW */
extern volatile long fw_last_need, fw_last_free; extern volatile int fw_last_cmd, fw_refused; extern volatile unsigned long fw_reserve_now;

#endif
