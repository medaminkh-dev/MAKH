/**
 * MakhOS - test_shell.c
 * The shell is driven by strings, so every command is testable: tokenizer
 * edge cases, status codes, argument validation, and real commands against
 * the live kernel (ping over loopback, heap integrity, ...).
 */

#include <ktest.h>
#include <shell.h>
#include <lib/string.h>

KTEST(shell, tokenizer_splits_on_runs_of_whitespace) {
    char buf[] = "  ping \t 1.2.3.4   3  ";
    char* argv[SHELL_MAX_ARGS];
    int argc = shell_tokenize(buf, argv, SHELL_MAX_ARGS);
    KEXPECT_EQ(argc, 3);
    KEXPECT_EQ(strcmp(argv[0], "ping"), 0);
    KEXPECT_EQ(strcmp(argv[1], "1.2.3.4"), 0);
    KEXPECT_EQ(strcmp(argv[2], "3"), 0);
}

KTEST(shell, tokenizer_caps_argc) {
    char buf[] = "a b c d e f g h i j k";
    char* argv[4];
    KEXPECT_EQ(shell_tokenize(buf, argv, 4), 4);
    KEXPECT_EQ(strcmp(argv[3], "d"), 0);
}

KTEST(shell, empty_and_blank_lines_are_noops) {
    KEXPECT_EQ(shell_exec(""), SHELL_OK);
    KEXPECT_EQ(shell_exec("    \t  "), SHELL_OK);
    KEXPECT_EQ(shell_exec(NULL), SHELL_OK);
}

KTEST(shell, unknown_command_is_127) {
    KEXPECT_EQ(shell_exec("definitely-not-a-command"), SHELL_NOTFOUND);
}

KTEST(shell, overlong_line_is_truncated_safely) {
    char line[400];
    for (int i = 0; i < 399; i++) line[i] = 'x';
    line[399] = '\0';
    KEXPECT_EQ(shell_exec(line), SHELL_NOTFOUND);   /* no overflow, no crash */
}

KTEST(shell, info_commands_succeed) {
    KEXPECT_EQ(shell_exec("help"), SHELL_OK);
    KEXPECT_EQ(shell_exec("echo hello   world"), SHELL_OK);
    KEXPECT_EQ(shell_exec("uptime"), SHELL_OK);
    KEXPECT_EQ(shell_exec("ps"), SHELL_OK);
    KEXPECT_EQ(shell_exec("ifconfig"), SHELL_OK);
    KEXPECT_EQ(shell_exec("arp"), SHELL_OK);
    KEXPECT_EQ(shell_exec("netstat"), SHELL_OK);
}

KTEST(shell, mem_and_heapcheck_report_healthy_heap) {
    KEXPECT_EQ(shell_exec("mem"), SHELL_OK);
    KEXPECT_EQ(shell_exec("heapcheck"), SHELL_OK);
}

KTEST(shell, ping_validates_arguments) {
    KEXPECT_EQ(shell_exec("ping"), SHELL_USAGE);
    KEXPECT_EQ(shell_exec("ping 1.2.3"), SHELL_USAGE);
    KEXPECT_EQ(shell_exec("ping 1.2.3.256"), SHELL_USAGE);
    KEXPECT_EQ(shell_exec("ping 127.0.0.1 0"), SHELL_USAGE);
    KEXPECT_EQ(shell_exec("ping 127.0.0.1 abc"), SHELL_USAGE);
    KEXPECT_EQ(shell_exec("ping 127.0.0.1 99999999999"), SHELL_USAGE);
}

KTEST(shell, ping_loopback_succeeds) {
    KEXPECT_EQ(shell_exec("ping 127.0.0.1 3"), SHELL_OK);
}
