/* fwmem.c - see fwmem.h. */
#include "fwmem.h"
#include <string.h>

static uint32_t w16(const uint8_t *d, int o) { return (uint32_t)d[o] | (uint32_t)d[o + 1] << 8; }
static uint32_t l32(const uint8_t *d, int o) { return w16(d, o) << 16 | w16(d, o + 2); }   /* high word first (0x1a8a swaps) */

int fw_bip_check(const uint8_t *cdb, int ncdb, const uint8_t *data, int ndata, uint32_t reserve, FwBip *r) {
    memset(r, 0, sizeof *r);
    if (ncdb < 4 || ndata < 34) return -1;
    int fl = cdb[3];
    r->cmd = (int)w16(data, 0);
    r->bpl_host = l32(data, 6); r->block = l32(data, 10);
    r->lines = w16(data, 0x0e); r->pixels = w16(data, 0x10); r->channels = w16(data, 0x12); r->bits = w16(data, 0x14);
    r->w26 = (fl & 1) ? w16(data, 0x1a) : 1;
    r->w28 = (fl & 8) ? w16(data, 0x1c) : (r->cmd == 1 ? 0x602 : r->cmd == 4 ? 0x702 : 0xffff);
    /* the firmware's own arithmetic (0x1b18): 32-bit products (MS-C long multiply), the rows rounded up with a 16-bit
       addition (cx = lines + w26 - 1 wraps at 65536, found by the review) */
    uint32_t bpl = (uint32_t)r->pixels * r->channels * (r->bits > 8 ? 2u : 1u);
    uint32_t w26 = r->w26;
    uint32_t rows = w26 ? ((r->lines + w26 - 1) & 0xFFFFu) / w26 : 0;
    r->bpl = bpl; r->T = (uint32_t)(bpl * rows);
    int resets = r->cmd == 1 || r->cmd == 2;
    r->free_before = resets ? FW_POOL : (reserve < FW_POOL ? FW_POOL - reserve : 0);
    r->ok = 1;
    /* refused before the memory rule (the firmware would crash or hang):
       - cmd outside 1..4;
       - division by zero: w26 (rows, all); for cmd 1/4 the host's bpl and block size (0xc3c) and the computed bpl
         (0x1c8c / 0x1d00) - stricter than needed for cmd 1, which the driver always fills;
       - for cmd 1/4 (the transfer loop 0x1192) an end code the firmware has no counter for (0x4de: 0x602, 0x702,
         0xa02 only) - it would write through a NULL pointer and the loop would never end.
       cmd 2/3 (load prescan / processing; driver paths SC_do_processing, send_PM - never seen in the traces) only
       get the memory rule (review). */
    if (r->cmd < 1 || r->cmd > 4) r->ok = 0;                       /* the firmware's sizing leaves its locals unset */
    if (!w26) r->ok = 0;
    if ((r->cmd == 1 || r->cmd == 4) && (!r->bpl_host || !r->block || !bpl)) r->ok = 0;
    if ((r->cmd == 1 || r->cmd == 4) && r->w28 != 0x602 && r->w28 != 0x702 && r->w28 != 0xa02) r->ok = 0;   /* only cmd 1/4 run the transfer loop 0x1192 */
    uint32_t A = r->cmd == 2 ? 0 : r->block, R = (uint32_t)r->T;   /* cmd 2: A taken as 0 (0xc7a) */
    if (r->ok && r->cmd == 4) {
        uint32_t fit = (uint32_t)((r->free_before / A) * (A / r->bpl_host) * r->bpl_host);
        if (fit < R) R = fit;
    }
    uint32_t A3 = (uint32_t)(3u * A);   /* 32-bit, as in the firmware */
    r->need = A3 > R ? A3 : R;
    if (r->need == 0 || r->need > r->free_before) r->ok = 0;
    r->reserve_after = resets && r->ok ? (uint32_t)r->need : reserve;
    return 0;
}
void fw_bip_set_cmd(uint8_t *data, int cmd) { data[0] = (uint8_t)(cmd & 0xFF); data[1] = (uint8_t)(cmd >> 8); }
