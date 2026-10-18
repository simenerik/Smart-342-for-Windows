/* fwmem_test.c - checks of fwmem.c against every distinct 0x80 block sent to Simen's scanner (hardware sessions
 * s2..s9, copied byte for byte from the SCSI traces) and the outcome the scanner showed: all accepted except the
 * s9 RAW of 2618 x 3783 (the scanner stopped answering). The sessions' order is replayed so the reserve of each
 * prescan carries into the next final scan, as in the firmware. Then the streamed-RAW case (the s9 block as cmd 4)
 * and edge cases of the cmd-4 sizing, with the numbers worked out by hand in the comments. */
#include "fwmem.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static int hex2(const char *s, uint8_t *o, int max) {
    int n = 0; while (s[0] && s[1] && n < max) { unsigned v; sscanf(s, "%2x", &v); o[n++] = (uint8_t)v; s += 2; } return n;
}
typedef struct { const char *sess, *cdb, *data; int expect_ok; } Vec;
static const Vec V[] = {
    { "s2", "800000192200", "01000000000000004c0b0000f870ba02e20103000c00000000000100020a850d1000", 1 },
    { "s3", "800000192200", "0100000000000000000c000000780002000203000c00000000000100020ac90d0c00", 1 },
    { "s3", "800000112200", "04000000000000008004040000adb0012001040008000000000001000000c90d0c00", 1 },
    { "s4", "800000192200", "0100000000000000000c000000780002000203000c00000000000100020ac90d0b00", 1 },
    { "s5", "800000192200", "0100000000000000000c000000780002000203000c00000000000100020a850d0a00", 1 },
    { "s5", "800000112200", "0400000000000000f0040400d0afc4013c01040008000000000001000000850d0c00", 1 },
    { "s6", "800000192200", "0100000000000000000c000000780002000203000c00000000000100020ac90d0a00", 1 },
    { "s6", "800000112200", "04000000000000004003040040af1d01d000040008000000000001000000c90d0c00", 1 },
    { "s7", "800000112200", "04000000000000007006040060ad11019c01040008000000000001000000c90d0c00", 1 },
    { "s8", "800000192200", "0100000000000000a2090000546011019b0103000c00000000000100020ac90d0a00", 1 },
    { "s8", "800000112200", "04000000000000009c010400fcae11019c01010008000000000001000000c90d0d00", 1 },
    { "s9", "800000192200", "01000000000000005c3d02009865c70e3a0a03000c00000000000100020ac90d0c00", 0 },
};

int main(void) {
    uint8_t cdb[16], d[64]; FwBip r; uint32_t reserve = 0;
    for (unsigned i = 0; i < sizeof V / sizeof *V; i++) {
        if (i == 0 || strcmp(V[i].sess, V[i - 1].sess)) reserve = 0;     /* each session starts with a freshly started controller */
        int nc = hex2(V[i].cdb, cdb, 16), nd = hex2(V[i].data, d, 64);
        fw_bip_check(cdb, nc, d, nd, reserve, &r);
        CHECK(r.ok == V[i].expect_ok, "%s cmd %d %u x %u x %u ch %u bit: T %llu, need %llu, free %llu -> %s (scanner: %s)", V[i].sess, r.cmd,
              r.pixels, r.lines, r.channels, r.bits, (unsigned long long)r.T, (unsigned long long)r.need, (unsigned long long)r.free_before,
              r.ok ? "accepted" : "would stop", V[i].expect_ok ? "accepted" : "stopped answering");
        CHECK(r.bpl == r.bpl_host, "%s firmware bytes per line %llu = host's %u", V[i].sess, (unsigned long long)r.bpl, r.bpl_host);
        if (r.ok) reserve = r.reserve_after;
    }
    /* field decoding, s9: 15708 bytes/line, block 157080, w28 0xa02 from the host (flags 0x19 has bit 3) */
    hex2(V[11].cdb, cdb, 16); hex2(V[11].data, d, 64); fw_bip_check(cdb, 6, d, 34, 0, &r);
    CHECK(r.bpl_host == 15708 && r.block == 157080 && r.w28 == 0xa02 && r.w26 == 1 && r.T == 59423364ull, "s9 decoded: bpl %u block %u w28 %x w26 %u T %llu", r.bpl_host, r.block, r.w28, r.w26, (unsigned long long)r.T);
    /* final scan flags 0x11 (bit 3 clear): end code = the firmware default for cmd 4, 0x702 */
    hex2(V[2].cdb, cdb, 16); hex2(V[2].data, d, 64); fw_bip_check(cdb, 6, d, 34, 0, &r);
    CHECK(r.w28 == 0x702, "s3 final scan end code default %x (expected 702)", r.w28);
    /* streamed RAW: the s9 block as cmd 4 after a 512 x 512 preview (reserve 1,572,864): free 5,505,024;
       fit = (5,505,024 / 157,080 = 35) x (157,080 / 15,708 = 10) x 15,708 = 5,497,800 < T -> need 5,497,800 -> accepted */
    hex2(V[11].cdb, cdb, 16); hex2(V[11].data, d, 64); fw_bip_set_cmd(d, 4);
    fw_bip_check(cdb, 6, d, 34, 1572864, &r);
    CHECK(r.ok && r.need == 5497800ull && r.reserve_after == 1572864, "streamed s9 RAW after a preview: need %llu of %llu free, reserve stays %u", (unsigned long long)r.need, (unsigned long long)r.free_before, r.reserve_after);
    /* after a 1.0 Mpx normal RAW (reserve 6,000,000): free 1,077,888 -> fit 6 blocks = 942,480; 3A = 471,240 -> ok */
    fw_bip_check(cdb, 6, d, 34, 6000000, &r);
    CHECK(r.ok && r.need == 942480ull, "streamed after a big reserve: need %llu (free %llu)", (unsigned long long)r.need, (unsigned long long)r.free_before);
    /* reserve 6,800,000: free 277,888 -> fit 1 block; 3A = 471,240 > free -> would stop */
    fw_bip_check(cdb, 6, d, 34, 6800000, &r);
    CHECK(!r.ok, "streamed with almost no free memory refused (need %llu > free %llu)", (unsigned long long)r.need, (unsigned long long)r.free_before);
    /* the area rule for cmd 1 (colour 12-bit, block of 10 lines): 1152 x 1024 is exactly the pool, one line more is not */
    uint8_t m[34]; memset(m, 0, 34); fw_bip_set_cmd(m, 1);
    uint32_t px = 1152, ln = 1024, bpl = px * 6, blk = bpl * 10;
    m[6] = (uint8_t)(bpl >> 16); m[7] = (uint8_t)(bpl >> 24); m[8] = (uint8_t)bpl; m[9] = (uint8_t)(bpl >> 8);
    m[10] = (uint8_t)(blk >> 16); m[11] = (uint8_t)(blk >> 24); m[12] = (uint8_t)blk; m[13] = (uint8_t)(blk >> 8);
    m[0x0e] = (uint8_t)ln; m[0x0f] = (uint8_t)(ln >> 8); m[0x10] = (uint8_t)px; m[0x11] = (uint8_t)(px >> 8); m[0x12] = 3; m[0x14] = 12; m[0x1a] = 1; m[0x1c] = 0x02; m[0x1d] = 0x0a;   /* end code 0xa02 */
    uint8_t c19[6] = { 0x80, 0, 0, 0x19, 0x22, 0 };
    fw_bip_check(c19, 6, m, 34, 0, &r);
    CHECK(r.ok && r.need == FW_POOL, "1152 x 1024 colour Max Detail = %llu bytes = the whole pool: accepted", (unsigned long long)r.need);
    ln = 1025; m[0x0e] = (uint8_t)ln; m[0x0f] = (uint8_t)(ln >> 8);
    fw_bip_check(c19, 6, m, 34, 0, &r);
    CHECK(!r.ok, "1152 x 1025: %llu bytes > pool: would stop", (unsigned long long)r.need);
    /* refusals added after the review: end code without a counter, cmd 2/3, zero pixels (computed bpl 0) */
    ln = 1024; m[0x0e] = (uint8_t)ln; m[0x0f] = (uint8_t)(ln >> 8);
    m[0x1c] = 0x34; m[0x1d] = 0x12; fw_bip_check(c19, 6, m, 34, 0, &r);
    CHECK(!r.ok, "end code 0x1234 (no counter in the firmware) refused");
    m[0x1c] = 0x02; m[0x1d] = 0x0a; fw_bip_set_cmd(m, 3); fw_bip_check(c19, 6, m, 34, 0, &r);
    CHECK(r.ok && r.need == FW_POOL, "cmd 3 gets the memory rule: T = the pool, nothing reserved -> accepted");
    fw_bip_check(c19, 6, m, 34, 6000000, &r);
    CHECK(!r.ok, "cmd 3 with a 6,000,000 reserve: T > free -> refused");
    fw_bip_set_cmd(m, 2); fw_bip_check(c19, 6, m, 34, 6000000, &r);
    CHECK(r.ok && r.need == FW_POOL && r.free_before == FW_POOL, "cmd 2 empties the pool first (A taken as 0): need T -> accepted");
    fw_bip_set_cmd(m, 5); fw_bip_check(c19, 6, m, 34, 0, &r);
    CHECK(!r.ok, "cmd 5 refused");
    fw_bip_set_cmd(m, 1);
    fw_bip_set_cmd(m, 1); m[0x10] = 0; m[0x11] = 0; fw_bip_check(c19, 6, m, 34, 0, &r);
    CHECK(!r.ok, "0 pixels (the firmware would divide by 0) refused");
    /* the firmware's 16-bit row rounding: lines 65535, w26 2 -> (65535 + 1) & 0xffff = 0 rows -> T 0 -> need = 3A */
    m[0x10] = 100; m[0x11] = 0; m[0x0e] = 0xff; m[0x0f] = 0xff; m[0x1a] = 2;
    fw_bip_check(c19, 6, m, 34, 0, &r);
    CHECK(r.T == 0, "16-bit row rounding as in the firmware: 65535 lines / 2 -> T %llu (0)", (unsigned long long)r.T);
    /* final scan after a 1,000,000-pixel RAW block (reserve 6,000,000): the driver's largest scan block, 307,200 bytes
       (SCSI_max_buf_for_scan 0x07e228), needs 3 x 307,200 = 921,600 <= 1,077,888 free -> accepted */
    uint8_t f[34]; memset(f, 0, 34); fw_bip_set_cmd(f, 4); uint32_t fb = 1200, fa = 256 * fb;
    f[6] = (uint8_t)(fb >> 16); f[7] = (uint8_t)(fb >> 24); f[8] = (uint8_t)fb; f[9] = (uint8_t)(fb >> 8);
    f[10] = (uint8_t)(fa >> 16); f[11] = (uint8_t)(fa >> 24); f[12] = (uint8_t)fa; f[13] = (uint8_t)(fa >> 8);
    f[0x0e] = 0x00; f[0x0f] = 0x10; f[0x10] = 0x2c; f[0x11] = 0x01; f[0x12] = 4; f[0x14] = 8; f[0x1a] = 1;   /* w26 1, as in every real block */
    uint8_t c11[6] = { 0x80, 0, 0, 0x11, 0x22, 0 };
    fw_bip_check(c11, 6, f, 34, 6000000, &r);
    CHECK(r.ok && r.need == 921600, "final scan (block 307,200) after a 1,000,000-pixel RAW: need %llu of %llu free", (unsigned long long)r.need, (unsigned long long)r.free_before);
    fw_bip_check(c11, 6, f, 34, 6291456, &r);
    CHECK(!r.ok, "the same after a 1,048,576-pixel RAW (0.1.19 draft default): refused (need %llu > free %llu) - why the block is 1,000,000", (unsigned long long)r.need, (unsigned long long)r.free_before);
    printf("%s (%d failed)\n", fails ? "FAIL" : "ALL PASS", fails);
    return fails != 0;
}
