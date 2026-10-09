/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* ifconfig: list the network interfaces (name, flags, addresses, MAC, counters)
 * via the MAKH-specific SYS_MAKH_IFINFO call. */
#include "usys.h"

static int putu(char* b, unsigned long v) {
    char t[20];
    int n = 0;
    if (!v) { b[0] = '0'; return 1; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) b[i] = t[n - 1 - i];
    return n;
}
static int puts_(char* b, const char* s) {
    int n = 0;
    while (s[n]) { b[n] = s[n]; n++; }
    return n;
}
static int emit_ip(char* b, unsigned int ip) {
    int p = 0;
    for (int i = 3; i >= 0; i--) {
        p += putu(b + p, (ip >> (i * 8)) & 0xFF);
        if (i) b[p++] = '.';
    }
    return p;
}
static int emit_mac(char* b, const unsigned char* m) {
    static const char* hex = "0123456789abcdef";
    int p = 0;
    for (int i = 0; i < 6; i++) {
        b[p++] = hex[(m[i] >> 4) & 0xF];
        b[p++] = hex[m[i] & 0xF];
        if (i < 5) b[p++] = ':';
    }
    return p;
}

int umain(void) {
    char b[320];
    int any = 0;
    for (int i = 0; ; i++) {
        struct makh_ifinfo info;
        if (umakh_ifinfo(i, &info) < 0) break;
        any = 1;
        int p = 0;
        for (int k = 0; k < 8 && info.name[k]; k++) b[p++] = info.name[k];
        p += puts_(b + p, ": flags=");
        p += puts_(b + p, (info.flags & MAKH_IF_UP) ? "UP" : "DOWN");
        if (info.flags & MAKH_IF_LOOPBACK) p += puts_(b + p, ",LOOPBACK");
        b[p++] = '\n';

        p += puts_(b + p, "    inet ");
        p += emit_ip(b + p, info.ip);
        p += puts_(b + p, "  netmask ");
        p += emit_ip(b + p, info.netmask);
        if (info.gateway) {
            p += puts_(b + p, "  gateway ");
            p += emit_ip(b + p, info.gateway);
        }
        b[p++] = '\n';

        if (!(info.flags & MAKH_IF_LOOPBACK)) {
            p += puts_(b + p, "    ether ");
            p += emit_mac(b + p, info.mac);
            b[p++] = '\n';
        }

        p += puts_(b + p, "    RX packets ");
        p += putu(b + p, info.rx_packets);
        p += puts_(b + p, "  bytes ");
        p += putu(b + p, info.rx_bytes);
        b[p++] = '\n';
        p += puts_(b + p, "    TX packets ");
        p += putu(b + p, info.tx_packets);
        p += puts_(b + p, "  bytes ");
        p += putu(b + p, info.tx_bytes);
        b[p++] = '\n';
        b[p++] = '\n';
        uwrite(1, b, (unsigned long)p);
    }
    if (!any) uwrite(2, "ifconfig: no interfaces\n", 24);
    return 0;
}
