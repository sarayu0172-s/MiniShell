/*
 * shell.h — Core shell definitions and shell-level interface
 */

#ifndef SHELL_H
#define SHELL_H

#include <sys/types.h>   /* pid_t */

/* ── compile-time tunables ───────────────────────────────────────────── */
#define SHELL_NAME      "myshell"
#define SHELL_MAX_INPUT 4096
#define MAX_HISTORY     500

/* ── ANSI colour escapes ─────────────────────────────────────────────── */
#define C_RESET         "\033[0m"
#define C_BOLD          "\033[1m"
#define C_GREEN         "\033[1;32m"
#define C_CYAN          "\033[1;36m"
#define C_YELLOW        "\033[1;33m"
#define C_RED           "\033[1;31m"
#define C_BLUE          "\033[1;34m"
#define C_MAGENTA       "\033[1;35m"

/* ── history ring buffer ─────────────────────────────────────────────── */
extern char *history[MAX_HISTORY];
extern int   history_count;

void history_push(const char *line);

/* ── background job registry ─────────────────────────────────────────── */
/* Called by executor.c when a background child is forked.               */
void bg_register(pid_t pid);

/* ── shell lifecycle ─────────────────────────────────────────────────── */
void shell_init(void);
void shell_loop(void);
void shell_cleanup(void);
void shell_exit(int code);

#endif /* SHELL_H */
