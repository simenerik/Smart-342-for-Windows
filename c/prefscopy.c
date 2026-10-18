/* prefscopy.c - Scanner(Flash Disk) --> Mac(Smart342.Prefs): the read-only parameter copy of the original
 * "Smart342(L) Calibration" application (File > Copy Parameters, second choice), done on files. C port of
 * scripts/flash_to_prefs.py (reverse engineered from Calibration 2.2.0, CODE 5):
 *   copy_params(2) -> load_from_flash(1): create_PrefsfileBackup("Smart342.Old"); load_from_flash_disk (SCP 0x13
 *   'flash14' -> Smart342.bin, check_size_and_chksum); copy_temp_str_to_Prefs: copy_temp_str_res(prefs, temp, id) for
 *   STR# 300, 400, 500, 401, 402, 403, 404, [399, 410 only if lw_butting_configured()], 1000, 600 - Get1Resource in the
 *   temp file; if present: RmveResource the old one in the prefs and AddResource the new data with the source's name,
 *   attributes 0. A resource missing from the flash file is skipped.
 * The flash file is the one the scanner sends (16-byte header: >I total length, >H signed-byte checksum of the resource
 * fork, 10 zero bytes; then the resource fork). Here it comes from the scanner software's own read at start-up
 * (hle_scsi.c flash_cap) - nothing is ever sent to the scanner for it. */
#include "mac.h"
#include "prefscopy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int ORDER[11] = { 300, 400, 500, 401, 402, 403, 404, 399, 410, 1000, 600 };

static uint32_t rd32b(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t rd16b(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* a resource fork whose maps and data lie inside the buffer (rsrc_parse itself does not check) */
static int fork_ok(const uint8_t *b, size_t n) {
    if (n < 16) return 0;
    uint32_t doff = rd32b(b), moff = rd32b(b + 4), dlen = rd32b(b + 8), mlen = rd32b(b + 12);
    if ((uint64_t)doff + dlen > n || (uint64_t)moff + mlen > n || mlen < 30) return 0;
    const uint8_t *m = b + moff; uint16_t tlo = rd16b(m + 24), nlo = rd16b(m + 26);
    if ((uint32_t)tlo + 2 > mlen || nlo > mlen) return 0;
    const uint8_t *tl = m + tlo; int ntypes = (int16_t)rd16b(tl) + 1;
    if (ntypes < 0 || ntypes > 1000 || (uint64_t)tlo + 2 + 8 * (uint64_t)ntypes > mlen) return 0;
    for (int t = 0; t < ntypes; t++) {
        const uint8_t *te = tl + 2 + 8 * t; int cnt = (int16_t)rd16b(te + 4) + 1; uint16_t ro = rd16b(te + 6);
        if (cnt < 0 || (uint64_t)tlo + ro + 12 * (uint64_t)cnt > mlen) return 0;
        for (int k = 0; k < cnt; k++) {
            const uint8_t *re = tl + ro + 12 * k; uint16_t no = rd16b(re + 2); uint32_t ao = rd32b(re + 4) & 0xFFFFFF;
            if ((uint64_t)ao + 4 > dlen || (uint64_t)ao + 4 + rd32b(b + doff + ao) > dlen) return 0;
            if (no != 0xFFFF && ((uint64_t)nlo + no + 1 > mlen || (uint64_t)nlo + no + 1 + m[nlo + no] > mlen)) return 0;
        }
    }
    return 1;
}

int flash_file_check(const uint8_t *f, size_t n, char *msg, int msgmax) {
    if (n < 16 + 16) { snprintf(msg, msgmax, "the scanner's parameter file is too short (%zu bytes)", n); return 0; }
    uint32_t total = rd32b(f); uint16_t ck = rd16b(f + 4); int32_t sum = 0;
    for (size_t i = 16; i < n; i++) sum += (int8_t)f[i];
    if (total != n) { snprintf(msg, msgmax, "the scanner's parameter file has %zu bytes, its header says %u", n, total); return 0; }
    if ((uint16_t)sum != ck) { snprintf(msg, msgmax, "the scanner's parameter file checksum %04x differs from its header (%04x)", (unsigned)(uint16_t)sum, ck); return 0; }
    for (int i = 6; i < 16; i++) if (f[i]) { snprintf(msg, msgmax, "the scanner's parameter file header is not as expected"); return 0; }
    if (!fork_ok(f + 16, n - 16)) { snprintf(msg, msgmax, "the scanner's parameter file is not a valid resource file"); return 0; }
    return 1;
}

/* string 1 of STR# 300 ("95171130/01   " + CR + "scanner id.") -> "95171130/01" */
static void serial_of(ResFile *rf, char *out, int outmax) {
    out[0] = 0; Res *r = resfile_get(rf, ostype_i("STR#"), 300);
    if (!r || r->data.n < 3) return;
    int L = r->data.b[2]; if ((size_t)L + 3 > r->data.n) return;
    int k = 0; for (int i = 0; i < L && k < outmax - 1; i++) { char c = (char)r->data.b[3 + i]; if (c == ' ' || c == '\r') break; out[k++] = c; } out[k] = 0;
}

int prefs_serial(const uint8_t *fork, size_t n, char *out, int outmax) {
    out[0] = 0; if (!fork_ok(fork, n)) return 0;
    ResFile *rf = resfile_new(0, "", fork, n, 0); serial_of(rf, out, outmax);
    for (int i = 0; i < rf->n; i++) { free(rf->r[i].name); bytes_free(&rf->r[i].data); } free(rf->r); free(rf->path); free(rf);
    return out[0] != 0;
}

static void rf_free(ResFile *rf) { if (!rf) return; for (int i = 0; i < rf->n; i++) { free(rf->r[i].name); bytes_free(&rf->r[i].data); } free(rf->r); free(rf->path); free(rf); }

int prefs_copy_from_flash(const uint8_t *flash, size_t nflash, const uint8_t *prefs, size_t nprefs, int lw_butting,
                          Bytes *out, char *flash_serial, char *prefs_serial_out, int sermax, char *msg, int msgmax) {
    memset(out, 0, sizeof *out); msg[0] = 0;
    if (!flash_file_check(flash, nflash, msg, msgmax)) return 0;
    if (!fork_ok(prefs, nprefs)) { snprintf(msg, msgmax, "Smart342.Prefs is not a valid resource file"); return 0; }
    ResFile *src = resfile_new(0, "", flash + 16, nflash - 16, 0), *dst = resfile_new(0, "", prefs, nprefs, 1);
    serial_of(src, flash_serial, sermax); serial_of(dst, prefs_serial_out, sermax);
    uint32_t T = ostype_i("STR#"); int done = 0; char list[200] = ""; int lk = 0;
    for (int i = 0; i < 11; i++) {
        int id = ORDER[i];
        if ((id == 399 || id == 410) && !lw_butting) continue;
        Res *r = resfile_get(src, T, id); if (!r) continue;
        resfile_remove(dst, T, id);
        resfile_put(dst, T, id, r->name, 0, r->data.b, r->data.n);
        done++; lk += snprintf(list + lk, sizeof list - lk, "%s%d", lk ? " " : "", id);
    }
    if (!resfile_get(src, T, 300)) { snprintf(msg, msgmax, "the scanner's parameter file has no scanner number (STR# 300)"); rf_free(src); rf_free(dst); return 0; }
    *out = resfile_serialize(dst);
    snprintf(msg, msgmax, "%d parameter resources copied (STR# %s)", done, list);
    rf_free(src); rf_free(dst);
    return done;
}

/* 0.3.1: replace resources of one type in a prefs resource fork (the clean gain vectors, GAIN 601 - 609) */
int prefs_put_resources(const uint8_t *prefs, size_t nprefs, const char *type, const int *ids, const char *const *names,
                        const uint8_t *const *data, const size_t *ndata, int n, Bytes *out, char *msg, int msgmax) {
    memset(out, 0, sizeof *out); msg[0] = 0;
    if (!fork_ok(prefs, nprefs)) { snprintf(msg, msgmax, "Smart342.Prefs is not a valid resource file"); return 0; }
    ResFile *dst = resfile_new(0, "", prefs, nprefs, 1); uint32_t T = ostype_i(type);
    for (int i = 0; i < n; i++) { resfile_remove(dst, T, ids[i]); resfile_put(dst, T, ids[i], names[i], 0, data[i], ndata[i]); }
    *out = resfile_serialize(dst); rf_free(dst);
    snprintf(msg, msgmax, "%d %s resource%s replaced", n, type, n == 1 ? "" : "s");
    return n;
}

/* 0.3.1: a copy of one resource's data (returns its length, -1 = none / not a valid file / too large for out) */
long prefs_get_resource(const uint8_t *prefs, size_t nprefs, const char *type, int id, uint8_t *out, size_t outmax) {
    if (!prefs || !fork_ok(prefs, nprefs)) return -1;
    ResFile *rf = resfile_new(0, "", prefs, nprefs, 0); Res *r = resfile_get(rf, ostype_i(type), id); long n = -1;
    if (r && r->data.n <= outmax) { memcpy(out, r->data.b, r->data.n); n = (long)r->data.n; }
    rf_free(rf); return n;
}
