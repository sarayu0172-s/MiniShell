/*
 * builtins.c — Implementation of shell built-in commands
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#include "builtins.h"
#include "shell.h"      /* history[], history_count, shell_exit() */

/* Forward declaration — builtin_help is defined after the table it prints. */
int builtin_help(const Command *cmd);

/* ── cd ─────────────────────────────────────────────────────────────── */
/*
 * cd [dir|-]
 *
 * Changes the shell's current working directory.
 *   • 'cd'        → change to $HOME
 *   • 'cd -'      → change to $OLDPWD  (previous directory)
 *   • 'cd <path>' → change to <path>
 *
 * Maintains OLDPWD and PWD environment variables.
 */
int builtin_cd(const Command *cmd)
{
    const char *target;
    char cwd[PATH_MAX];

    /* Record the current directory before changing it. */
    if (getcwd(cwd, sizeof cwd) == NULL) {
        perror(SHELL_NAME ": cd: getcwd");
        return 1;
    }

    if (cmd->args[1] == NULL) {
        /* No argument → go home. */
        target = getenv("HOME");
        if (!target || *target == '\0') {
            fprintf(stderr, SHELL_NAME ": cd: HOME not set\n");
            return 1;
        }
    } else if (strcmp(cmd->args[1], "-") == 0) {
        /* 'cd -' → go to previous directory. */
        target = getenv("OLDPWD");
        if (!target || *target == '\0') {
            fprintf(stderr, SHELL_NAME ": cd: OLDPWD not set\n");
            return 1;
        }
        printf("%s\n", target);
    } else {
        target = cmd->args[1];
    }

    if (chdir(target) != 0) {
        fprintf(stderr, SHELL_NAME ": cd: %s: %s\n", target, strerror(errno));
        return 1;
    }

    /* Update OLDPWD and PWD in the environment. */
    setenv("OLDPWD", cwd, 1);

    char new_cwd[PATH_MAX];
    if (getcwd(new_cwd, sizeof new_cwd) != NULL)
        setenv("PWD", new_cwd, 1);

    return 0;
}

/* ── exit ───────────────────────────────────────────────────────────── */
/*
 * exit [code]
 *
 * Cleanly terminates the shell.  The optional integer argument sets the
 * exit status; defaults to 0.
 */
int builtin_exit(const Command *cmd)
{
    int code = 0;
    if (cmd->args[1]) {
        char *end;
        long val = strtol(cmd->args[1], &end, 10);
        if (*end != '\0') {
            fprintf(stderr, SHELL_NAME ": exit: '%s': numeric argument required\n",
                    cmd->args[1]);
            return 1;
        }
        code = (int)val;
    }
    shell_exit(code);
    return 0;   /* unreachable */
}

/* ── pwd ────────────────────────────────────────────────────────────── */
int builtin_pwd(const Command *cmd)
{
    (void)cmd;
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd) == NULL) {
        perror(SHELL_NAME ": pwd");
        return 1;
    }
    printf("%s\n", cwd);
    return 0;
}

/* ── history ────────────────────────────────────────────────────────── */
/*
 * history [n]
 *
 * Displays previously entered commands with a sequential index.
 * The optional argument limits output to the last N entries.
 */
int builtin_history(const Command *cmd)
{
    int total = (history_count < MAX_HISTORY) ? history_count : MAX_HISTORY;

    int limit = total;
    if (cmd->args[1]) {
        char *end;
        long val = strtol(cmd->args[1], &end, 10);
        if (*end != '\0' || val <= 0) {
            fprintf(stderr, SHELL_NAME ": history: '%s': positive number required\n",
                    cmd->args[1]);
            return 1;
        }
        if ((int)val < total) limit = (int)val;
    }

    int start_logical = history_count - limit;

    for (int i = 0; i < limit; ++i) {
        int logical = start_logical + i;
        int slot    = logical % MAX_HISTORY;
        if (history[slot])
            printf(C_YELLOW "  %4d" C_RESET "  %s\n", logical + 1, history[slot]);
    }
    return 0;
}

/* ── registry ────────────────────────────────────────────────────────
 * Defined BEFORE builtin_help so the help function can iterate it.
 * Terminated with a {NULL,...} sentinel.
 */
static const BuiltinEntry builtins[] = {
    {
        "cd",
        builtin_cd,
        "cd [dir|-]",
        "Change the current working directory."
    },
    {
        "exit",
        builtin_exit,
        "exit [code]",
        "Exit the shell with an optional status code."
    },
    {
        "help",
        builtin_help,
        "help",
        "Display this help message."
    },
    {
        "pwd",
        builtin_pwd,
        "pwd",
        "Print the current working directory."
    },
    {
        "history",
        builtin_history,
        "history [n]",
        "Show command history (optionally last n entries)."
    },
    /* sentinel */
    { NULL, NULL, NULL, NULL }
};

/* ── help ───────────────────────────────────────────────────────────── */
/*
 * Defined after the table so it can iterate builtins[].
 */
int builtin_help(const Command *cmd)
{
    (void)cmd;

    printf("\n" C_BOLD C_CYAN
           "  ┌─────────────────────────────────────────────────────┐\n"
           "  │           " SHELL_NAME " — built-in commands                    │\n"
           "  └─────────────────────────────────────────────────────┘\n"
           C_RESET "\n");

    for (int i = 0; builtins[i].name != NULL; ++i) {
        printf("  " C_GREEN "%-12s" C_RESET " %s\n"
               "               " C_YELLOW "usage: %s" C_RESET "\n\n",
               builtins[i].name,
               builtins[i].description,
               builtins[i].usage);
    }

    printf("  " C_CYAN "Other features:" C_RESET "\n"
           "    Pipes:        cmd1 | cmd2 | cmd3\n"
           "    Redirect in:  cmd < file\n"
           "    Redirect out: cmd > file   or   cmd >> file\n"
           "    Background:   cmd &\n\n");
    return 0;
}

/* ── public API ─────────────────────────────────────────────────────── */

const BuiltinEntry *builtins_lookup(const char *name)
{
    for (int i = 0; builtins[i].name != NULL; ++i) {
        if (strcmp(builtins[i].name, name) == 0)
            return &builtins[i];
    }
    return NULL;
}

int builtins_exec(const Command *cmd)
{
    if (!cmd || !cmd->args[0])
        return -1;

    const BuiltinEntry *entry = builtins_lookup(cmd->args[0]);
    if (!entry)
        return -1;

    return entry->handler(cmd);
}

const BuiltinEntry *builtins_list(void)
{
    return builtins;
}
