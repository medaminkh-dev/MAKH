/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - shell.c
 * Kernel command shell (Phase 14).
 *
 * =============================================================================
 * Design
 * =============================================================================
 * A static table of commands, each an ordinary C function taking argc/argv and
 * returning a status. No allocation, no globals touched by the parser: the
 * line is copied into a stack buffer and split in place. That keeps the shell
 * safe to drive from tests and from the fuzzer with arbitrary bytes.
 *
 * Commands that touch the network take net_lock themselves, exactly like any
 * other client of the stack.
 * =============================================================================
 */

#include <shell.h>
#include <kernel.h>
#include <klog.h>
#include <vga.h>
#include <sched.h>
#include <ktime.h>
#include <ktest.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <mm/pmm.h>
#include <net/net.h>
#include <kfuzz.h>
#include <drivers/timer.h>

/* -------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* -------------------------------------------------------------------------- */

/* Parse a decimal unsigned; returns 0 on success. */
static int parse_uint(const char* s, uint32_t* out) {
    if (!s || !*s) return -1;
    uint64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (uint64_t)(*s - '0');
        if (v > 0xFFFFFFFFu) return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

int shell_tokenize(char* buf, char** argv, int max) {
    int argc = 0;
    char* p = buf;
    while (*p && argc < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

/* -------------------------------------------------------------------------- */
/* Commands                                                                   */
/* -------------------------------------------------------------------------- */

static int cmd_help(int argc, char** argv);

static int cmd_uptime(int argc, char** argv) {
    (void)argc; (void)argv;
    uint64_t ms = clock_now_ms();
    kprintf("up %lu.%03lu s (%lu ticks)\n", (unsigned long)(ms / 1000),
            (unsigned long)(ms % 1000), (unsigned long)timer_get_ticks());
    return SHELL_OK;
}

static int cmd_clear(int argc, char** argv) {
    (void)argc; (void)argv;
    terminal_clear();
    return SHELL_OK;
}

static int cmd_mem(int argc, char** argv) {
    (void)argc; (void)argv;
    int bad = kheap_check();
    kprintf("heap:  used %lu  free %lu  total %lu bytes\n",
            (unsigned long)kheap_get_used(), (unsigned long)kheap_get_free(),
            (unsigned long)kheap_get_total());
    kprintf("       integrity %s", bad ? "CORRUPT" : "ok");
    if (bad) kprintf(" (first bad block %p)", (void*)kheap_check_bad_block());
    kprintf(", rejected frees %lu\n", (unsigned long)kheap_get_bad_frees());
    kprintf("frames: free %lu / %lu KiB\n",
            (unsigned long)(pmm_get_free_memory() / 1024),
            (unsigned long)(pmm_get_total_memory() / 1024));
    return bad ? SHELL_ERR : SHELL_OK;
}

static int cmd_ps(int argc, char** argv) {
    (void)argc; (void)argv;
    sched_debug_dump();
    return SHELL_OK;
}

static int cmd_ifconfig(int argc, char** argv) {
    (void)argc; (void)argv;
    char ip[16], mask[16], gw[16];
    for (int i = 0; i < netdev_count(); i++) {
        netdev_t* d = netdev_get(i);
        kprintf("%s: %s%s\n", d->name, d->up ? "UP" : "DOWN",
                d->is_loopback ? " LOOPBACK" : "");
        if (!d->is_loopback) {
            kprintf("    ether %02x:%02x:%02x:%02x:%02x:%02x\n", d->mac[0], d->mac[1],
                    d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
        }
        kprintf("    inet %s  netmask %s", ip_to_str(d->ip, ip), ip_to_str(d->netmask, mask));
        if (d->gateway) kprintf("  gateway %s", ip_to_str(d->gateway, gw));
        kprintf("\n    RX packets %lu bytes %lu dropped %lu\n", (unsigned long)d->rx_packets,
                (unsigned long)d->rx_bytes, (unsigned long)d->rx_dropped);
        kprintf("    TX packets %lu bytes %lu dropped %lu\n", (unsigned long)d->tx_packets,
                (unsigned long)d->tx_bytes, (unsigned long)d->tx_dropped);
    }
    return SHELL_OK;
}

static int cmd_arp(int argc, char** argv) {
    (void)argc; (void)argv;
    net_lock();
    arp_cache_dump();
    net_unlock();
    return SHELL_OK;
}

static int cmd_netstat(int argc, char** argv) {
    (void)argc; (void)argv;
    net_lock();
    tcp_debug_dump();
    net_unlock();
    kprintf("ip rx %lu tx %lu bad %lu noroute %lu | tcp rx %lu tx %lu retrans %lu rst %lu"
            " | udp rx %lu tx %lu | icmp rx %lu tx %lu\n",
            (unsigned long)net_stats.ip_rx, (unsigned long)net_stats.ip_tx,
            (unsigned long)net_stats.ip_bad, (unsigned long)net_stats.dropped_no_route,
            (unsigned long)net_stats.tcp_rx, (unsigned long)net_stats.tcp_tx,
            (unsigned long)net_stats.tcp_retrans, (unsigned long)net_stats.tcp_rst_tx,
            (unsigned long)net_stats.udp_rx, (unsigned long)net_stats.udp_tx,
            (unsigned long)net_stats.icmp_rx, (unsigned long)net_stats.icmp_tx);
    return SHELL_OK;
}

static int cmd_ping(int argc, char** argv) {
    uint32_t dst, count = 4;
    if (argc < 2 || ip_from_str(argv[1], &dst) != 0) return SHELL_USAGE;
    if (argc >= 3 && (parse_uint(argv[2], &count) != 0 || count == 0 || count > 100))
        return SHELL_USAGE;

    char ip[16];
    ip_to_str(dst, ip);
    kprintf("PING %s: %u echo requests\n", ip, count);
    uint32_t ok = 0;
    for (uint32_t seq = 1; seq <= count; seq++) {
        net_lock();
        int rtt = icmp_ping(dst, (uint16_t)seq, 1000);
        net_unlock();
        if (rtt >= 0) {
            ok++;
            kprintf("reply from %s: seq=%u time=%d ms\n", ip, seq, rtt);
        } else {
            kprintf("seq=%u: no reply (%d)\n", seq, rtt);
        }
    }
    kprintf("--- %s: %u sent, %u received, %u%% loss\n", ip, count, ok,
            (count - ok) * 100 / count);
    return ok ? SHELL_OK : SHELL_ERR;
}

static int cmd_heapcheck(int argc, char** argv) {
    (void)argc; (void)argv;
    int bad = kheap_check();
    if (bad) {
        kprintf("heap CORRUPT at %p\n", (void*)kheap_check_bad_block());
        return SHELL_ERR;
    }
    kprintf("heap ok\n");
    return SHELL_OK;
}

static int cmd_selftest(int argc, char** argv) {
    int fails = argc >= 2 ? ktest_run_suite(argv[1]) : ktest_run_all();
    kprintf("selftest: %d failing test(s)\n", fails);
    return fails ? SHELL_ERR : SHELL_OK;
}

static int cmd_echo(int argc, char** argv) {
    for (int i = 1; i < argc; i++) kprintf(i > 1 ? " %s" : "%s", argv[i]);
    kprintf("\n");
    return SHELL_OK;
}

static int cmd_reboot(int argc, char** argv) {
    (void)argc; (void)argv;
    kprintf("rebooting...\n");
    /* Pulse the CPU reset line through the 8042 keyboard controller. */
    for (int i = 0; i < 100000 && (inb(0x64) & 0x02); i++) { }
    outb(0x64, 0xFE);
    return SHELL_ERR;   /* still here: reset didn't happen */
}

static uint32_t fuzz_target_bit(const char* name) {
    int n; const kfuzz_target_t* t = kfuzz_targets(&n);
    for (int i = 0; i < n; i++) if (strcmp(name, t[i].name) == 0) return t[i].bit;
    return 0;
}

static int cmd_fuzz(int argc, char** argv) {
    /* fuzz [iters]                 - campaign over all targets
     * fuzz <target> [iters]        - campaign over one target
     * fuzz replay <seed> <target>  - reproduce one crashing seed          */
    if (argc >= 2 && strcmp(argv[1], "replay") == 0) {
        if (argc < 4) return SHELL_USAGE;
        uint32_t seed = 0;
        for (const char* p = argv[2]; *p; p++) {          /* hex or dec */
            if (p == argv[2] && p[0] == '0' && (p[1]=='x'||p[1]=='X')) { p++; continue; }
            char c = *p; uint32_t d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return SHELL_USAGE;
            seed = seed * 16 + d;
        }
        uint32_t bit = fuzz_target_bit(argv[3]);
        if (!bit) { kprintf("unknown target '%s'\n", argv[3]); return SHELL_USAGE; }
        int rc = kfuzz_replay(bit, seed, 64);
        kprintf("replay seed=0x%x target=%s -> %s\n", seed, argv[3],
                rc == 0 ? "clean" : rc == 1 ? "CRASH" : "ORACLE FAIL");
        return rc ? SHELL_ERR : SHELL_OK;
    }

    uint32_t mask = KFUZZ_T_ALL;
    uint64_t iters = 2000;
    int ai = 1;
    if (argc > ai) {
        uint32_t bit = fuzz_target_bit(argv[ai]);
        if (bit) { mask = bit; ai++; }
    }
    if (argc > ai) {
        uint32_t v;
        if (parse_uint(argv[ai], &v) != 0) return SHELL_USAGE;
        iters = v;
    }

    kfuzz_result_t res;
    kprintf("fuzzing %lu iterations...\n", (unsigned long)iters);
    int fails = kfuzz_run(mask, iters, 0, &res);
    kprintf("done: %lu iters, %lu crashes, %lu oracle-fails, coverage %lu edges, "
            "corpus %lu\n", (unsigned long)res.iterations, (unsigned long)res.crashes,
            (unsigned long)res.oracle_fails, (unsigned long)res.coverage,
            (unsigned long)res.corpus);
    if (fails) kprintf("last crash seed 0x%lx (vector %d)\n",
                       (unsigned long)res.last_crash_seed, res.last_crash_vector);
    return fails ? SHELL_ERR : SHELL_OK;
}

static const shell_cmd_t commands[] = {
    { "help",      "help",               "list commands",                     cmd_help },
    { "echo",      "echo [words...]",    "print the arguments",               cmd_echo },
    { "clear",     "clear",              "clear the screen",                  cmd_clear },
    { "uptime",    "uptime",             "time since boot",                   cmd_uptime },
    { "mem",       "mem",                "heap/frame usage + integrity check", cmd_mem },
    { "heapcheck", "heapcheck",          "walk and validate every heap block", cmd_heapcheck },
    { "ps",        "ps",                 "list threads",                      cmd_ps },
    { "ifconfig",  "ifconfig",           "network interfaces and counters",   cmd_ifconfig },
    { "arp",       "arp",                "ARP cache",                         cmd_arp },
    { "netstat",   "netstat",            "TCP connections + protocol stats",  cmd_netstat },
    { "ping",      "ping <ip> [count]",  "ICMP echo",                         cmd_ping },
    { "selftest",  "selftest [suite]",   "run the in-kernel test suite",      cmd_selftest },
    { "fuzz",      "fuzz [target] [n] | fuzz replay <seed> <target>", "self-fuzz the kernel", cmd_fuzz },
    { "reboot",    "reboot",             "reset the machine",                 cmd_reboot },
};
#define NCOMMANDS (sizeof(commands) / sizeof(commands[0]))

static int cmd_help(int argc, char** argv) {
    (void)argc; (void)argv;
    for (size_t i = 0; i < NCOMMANDS; i++)
        kprintf("  %-20s %s\n", commands[i].usage, commands[i].help);
    return SHELL_OK;
}

/* -------------------------------------------------------------------------- */
/* Entry point                                                                */
/* -------------------------------------------------------------------------- */

int shell_exec(const char* line) {
    if (!line) return SHELL_OK;

    char buf[SHELL_MAX_LINE];
    size_t n = 0;
    while (line[n] && n < sizeof(buf) - 1) { buf[n] = line[n]; n++; }
    buf[n] = '\0';

    char* argv[SHELL_MAX_ARGS];
    int argc = shell_tokenize(buf, argv, SHELL_MAX_ARGS);
    if (argc == 0) return SHELL_OK;

    for (size_t i = 0; i < NCOMMANDS; i++) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            int rc = commands[i].fn(argc, argv);
            if (rc == SHELL_USAGE) kprintf("usage: %s\n", commands[i].usage);
            return rc;
        }
    }
    kprintf("%s: command not found (try 'help')\n", argv[0]);
    return SHELL_NOTFOUND;
}
