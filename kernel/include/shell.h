/**
 * MakhOS - shell.h
 * The kernel command shell.
 *
 * shell_exec() is the whole shell: split a line into argv, find the command,
 * run it. The interactive REPL in kernel_main only feeds it lines, which makes
 * every command scriptable from ktests (and from the self-fuzzer).
 */

#ifndef MAKHOS_SHELL_H
#define MAKHOS_SHELL_H

#define SHELL_MAX_ARGS   8
#define SHELL_MAX_LINE   128

#define SHELL_OK         0
#define SHELL_ERR        1      /* command ran and failed          */
#define SHELL_USAGE      2      /* bad arguments                   */
#define SHELL_NOTFOUND   127    /* like sh: command not found      */

typedef int (*shell_cmd_fn)(int argc, char** argv);

typedef struct shell_cmd {
    const char*  name;
    const char*  usage;
    const char*  help;
    shell_cmd_fn fn;
} shell_cmd_t;

/* Execute one command line. Returns the command's status (SHELL_*). The
 * line is copied, so the caller's buffer is left untouched. */
int shell_exec(const char* line);

/* Split `buf` in place on spaces/tabs. Returns argc (<= max). */
int shell_tokenize(char* buf, char** argv, int max);

#endif /* MAKHOS_SHELL_H */
