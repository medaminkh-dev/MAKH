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
    default:      snprintf(buf, n, "imm:0");            break;
    }
}

static void emit_ir(unsigned long seed) {
    const u3_program* p = u3g_build(seed);
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
    for (int i = 0; i < p->ncalls; i++) {
        const u3_call* c = &p->calls[i];
        char tmp[32];
        printf("call %s", U3G_NAME[c->sysno]);
        for (int a = 0; a < c->nargs; a++) { op_str(&c->args[a], tmp, sizeof(tmp)); printf(" %s", tmp); }
        if (c->bind == U3_BIND_FD)   printf(" -> fd:%d", c->bind_h0);
        if (c->bind == U3_BIND_ADDR) printf(" -> addr:%d", c->bind_h0);
        printf("\n");
    }
    printf("\n");
}

int main(int argc, char** argv) {
    unsigned long start = argc > 1 ? strtoul(argv[1], 0, 10) : 0;
    unsigned long count = argc > 2 ? strtoul(argv[2], 0, 10) : 1;
    for (unsigned long i = 0; i < count; i++) {
        printf("### seed %lu\n", start + i);
        emit_ir(start + i);
    }
    return 0;
}
