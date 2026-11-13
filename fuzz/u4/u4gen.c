/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/*
 * MAKH UFUZZ U4 — host serializer for the shared typed-program generator.
 *
 * Compiles user/ufuzz_u3_gen.h (the SAME generator the guest /bin/ufuzz `cov`
 * mode runs) on the host and prints each generated program as U3 IR text, so
 * fuzz/u4/validate.py can run every one through the Python U3 validator — the
 * structural authority. QEMU is not involved, so the CI check is deterministic
 * and fail-closed.
 *
 *   u4gen <start-seed> <count>    # prints `count` programs, seeds start..start+count-1
 *
 * Each program is preceded by a `### seed N` attribution marker (a comment the
 * U3 deserializer ignores) so validate.py can split and blame a specific seed.
 */
#include <stdio.h>
#include <stdlib.h>
#include "ufuzz_u3_gen.h"

static void op_str(const u3_arg* a, char* buf, size_t n) {
    switch (a->kind) {
    case U3_IMM:  snprintf(buf, n, "imm:%ld", a->val);  break;
    case U3_RES:  snprintf(buf, n, "res:%ld", a->val);  break;
    case U3_NIL:  snprintf(buf, n, "null");             break;
    case U3_BUF:  snprintf(buf, n, "buf:%ld", a->val);  break;
    case U3_PATH: snprintf(buf, n, "path:%ld", a->val); break;
    case U3_IOV:  snprintf(buf, n, "iov:%ld", a->val);  break;
    case U3_FDSLOT: snprintf(buf, n, "slot:%ld", a->val); break;
    case U3_BADPTR: snprintf(buf, n, "bad:%ld", a->val);  break;
    default:      snprintf(buf, n, "imm:0");            break;
    }
}

static void emit_ir(unsigned long seed, unsigned long mut, int mode) {
    /* base==mut here so one seed sweeps both the skeleton and the selector/offset
     * space; the Python validator must accept every resulting program. mode 0 =
     * skeleton menu generator, mode 1 = ext2 block-map growth (direct/single/
     * double-indirect tiers), mode 2 = ext2 direct-tier control (U4.2 P1). */
    const u3_program* p = (mode == 1) ? u3g_build_ext2(seed, mut, 3) :
                          (mode == 2) ? u3g_build_ext2(seed, mut, 1) :
                          (mode == 3) ? u3g_build_emfile(U3G_FD_BUDGET) :
                          (mode == 4) ? u3g_build_emfile(U3G_FD_BUDGET - 1) :
                          (mode == 5) ? u3g_build_fdops(seed, mut) :
                          (mode == 6) ? u3g_build_piperb() :
                                        u3g_build(seed, mut);
    printf("abi 1\n");
    printf("prng lcg %lu\n", seed);
    printf("handles");
    if (p->nhandles == 0) printf(" -");
    else for (int i = 0; i < p->nhandles; i++) printf(" %s", u3g_handle_rtype[i]);
    printf("\n");
    for (int b = 0; b < p->nbufs; b++) {
        printf("buf %d %d ", b, p->bufs[b].len);
        for (int k = 0; k < p->bufs[b].len; k++) printf("%02x", p->bufs[b].data[k]);
        printf("\n");
    }
    for (int i = 0; i < p->npaths; i++) printf("path %d %s\n", i, p->paths[i]);
    /* U5.3: the baseline precondition, so an independent deriver knows how many
     * slots the program may assume (absent = no assumption). */
    if (p->need_free) printf("need %d %s\n", p->need_free, p->need_exact ? "exact" : "min");
    for (int i = 0; i < p->ncalls; i++) {
        const u3_call* c = &p->calls[i];
        char tmp[32];
        printf("call %s", U3G_NAME[c->sysno]);
        for (int a = 0; a < c->nargs; a++) { op_str(&c->args[a], tmp, sizeof(tmp)); printf(" %s", tmp); }
        if (c->bind == U3_BIND_FD)   printf(" -> fd:%d", c->bind_h0);
        if (c->bind == U3_BIND_ADDR) printf(" -> addr:%d", c->bind_h0);
        if (c->bind == U3_BIND_PIPE) printf(" -> fd:%d,%d", c->bind_h0, c->bind_h1);
        /* U5.3: the C generator's expectation, as an annotation the Python
         * deriver compares against but never uses to make its own prediction. */
        switch (c->expect_mode) {
        case U3_NONNEG: printf(" =nonneg"); break;
        case U3_NEG:    printf(" =neg"); break;
        case U3_EXACT:  printf(" =exact:%ld", c->expect_val); break;
        case U3_SLOT:   printf(" =slot:%ld", c->expect_val); break;
        case U3_BASEFL: printf(" =basefl:%ld", c->expect_val); break;
        default:        printf(" =any"); break;
        }
        printf("\n");
    }
    printf("\n");
}

int main(int argc, char** argv) {
    unsigned long start = argc > 1 ? strtoul(argv[1], 0, 10) : 0;
    unsigned long count = argc > 2 ? strtoul(argv[2], 0, 10) : 1;
    int mode = argc > 3 ? (int)strtoul(argv[3], 0, 10) : 0;   /* 0 skeleton, 1 ext2 */
    /* Optional 4th arg: an explicit mut_seed, to dump the exact (base, mut)
     * program a campaign reported. Without it mut == base (the sweep). */
    int has_mut = argc > 4;
    unsigned long mut = has_mut ? strtoul(argv[4], 0, 10) : 0;
    for (unsigned long i = 0; i < count; i++) {
        printf("### seed %lu\n", start + i);
        emit_ir(start + i, has_mut ? mut : start + i, mode);
    }
    return 0;
}
