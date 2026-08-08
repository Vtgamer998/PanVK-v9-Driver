/*
 * test_frag_bounds.c - CPU-only validation of the Valhall fragment job chain
 * packing (v9_pack_frag_job_chain) against the v9.xml genxml layout.
 *
 * The Fragment Job aggregate is:
 *   Header  : words 0-7  (Type@4:1, Index@4:16, Next@6)
 *   Payload : words 8-15 (BoundMin@8, BoundMax@9, Framebuffer@10, TileMap@12)
 *
 * In particular, Payload word 1 = Bound Max X (12 bits) / Bound Max Y (12 bits)
 * in 16x16 tile units, inclusive (max = size-1).  The old driver hardcoded it
 * to 0 (render only tile 0,0) and to 0x00030003 (only valid for 64x64).
 *
 * No GPU access, no /dev/mali0.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "v9_pack.h"

#define PACK_OFFSET 0xE380u

static int failures = 0;

static void check(const char *what, int ok) {
    if (!ok) {
        failures++;
        printf("  [FAIL] %s\n", what);
    }
}

static void run_size(const char *name, uint32_t w, uint32_t h) {
    uint32_t fj1[32], fj2[32];
    uint64_t mfbd1 = PACK_OFFSET + 0x00;
    uint64_t mfbd2 = PACK_OFFSET + 0x100;
    uint64_t fj2g  = PACK_OFFSET + 0x200;
    uint32_t exp_bound = (((w - 1) >> 4) | (((h - 1) >> 4) << 16));

    v9_pack_frag_job_chain(fj1, fj2, mfbd1, mfbd2, fj2g, w, h);

    printf("== %s %ux%u (exp bound 0x%08x) ==\n", name, w, h, exp_bound);

    /* FJ1 header */
    check("FJ1 type=9 index=1", fj1[4] == 0x00010012u);
    check("FJ1 Next -> FJ2", fj1[6] == (uint32_t)fj2g && fj1[7] == (uint32_t)(fj2g >> 32));
    check("FJ1 framebuffer ptr mfbd1|0x01",
          fj1[10] == (uint32_t)(mfbd1 | 0x01u) && fj1[11] == (uint32_t)(mfbd1 >> 32));
    /* FJ1 payload bound must span the whole frame (not tile 0,0) */
    check("FJ1 Bound Max = full frame", fj1[9] == exp_bound);
    printf("  FJ1[9] = 0x%08x\n", fj1[9]);

    /* FJ2 header */
    check("FJ2 type=9 index=2", fj2[4] == 0x00020012u);
    check("FJ2 Next = NULL", fj2[6] == 0 && fj2[7] == 0);
    check("FJ2 framebuffer ptr mfbd2|0x03",
          fj2[10] == (uint32_t)(mfbd2 | 0x03u) && fj2[11] == (uint32_t)(mfbd2 >> 32));
    check("FJ2 Bound Max = full frame", fj2[9] == exp_bound);
    printf("  FJ2[9] = 0x%08x\n", fj2[9]);

    /* Sanity: payload words 8, 12-15 must stay zero */
    check("FJ1 BoundMin stays 0", fj1[8] == 0);
    check("FJ2 BoundMin stays 0", fj2[8] == 0);
}

int main(void) {
    run_size("single-tile", 16, 16);
    run_size("2x2 tiles", 32, 32);
    run_size("4x4 tiles", 64, 64);
    run_size("non-square", 128, 96);
    run_size("HD 800x600", 800, 600);
    run_size("FHD 1920x1080", 1920, 1080);

    printf("\n%s\n", failures ? "test_frag_bounds: FAILED" : "test_frag_bounds: PASSED CLEANLY");
    return failures ? 1 : 0;
}
