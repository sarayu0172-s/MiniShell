/*
 * shell.c — Core shell loop, signal handling, and history management
 *
 * Responsibilities
 * ────────────────
 *   shell_init()    — install signal handlers, print the welcome banner.
 *   shell_loop()    — the Read-Eval-Print Loop (REPL); never returns.
 *   shell_exit()    — orderly shutdown called by the 'exit' built-in.
 *   shell_cleanup() — free heap memory; useful for Valgrind runs.
 *   history_push()  — append a command string to the ring buffer.
 *
 * Signal strategy
 * ───────────────
 *   SIGINT  (Ctrl-C):  The shell ignores it (SIG_IGN).  Each child process
 *                      inherits SIG_IGN through fork() but executor.c
 *                      immediately restores SIG_DFL in the child before
 *                      exec so that Ctrl-C actually kills the foreground
 *                      program.
 *
 *   SIGQUIT (Ctrl-\):  Same strategy as SIGINT.
 *
 *   SIGTSTP (Ctrl-Z):  Ignored at the shell level for now.  A full job-
 *                      control implementation would handle this properly,
 *                      but that is out of scope for this project.
 *
 *   SIGCHLD:           We install a handler that calls waitpid() with
 *                      WNOHANG in a loop.  This reaps background children
 *                      automatically as soon as they exit, preventing
 *                      zombie processes from accumulating.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <limits.h>

#include "shell.h"
#include "parser.h"
#include "executor.h"

/* ── history ring buffer ─────────────────────────────────────────────── */
char *history[MAX_HISTORY];
int   history_count = 0;

void history_push(const char *line)
{
    if (!line || *line == '\0') return;

    int slot = history_count % MAX_HISTORY;
    free(history[slot]);                 /* free old entry if ring is full */
    history[slot] = strdup(line);
    if (!history[slot]) return;          /* strdup failure: silently skip  */
    ++history_count;
}

/* ── background job registry ─────────────────────────────────────────────
 * The executor registers background PIDs here.  The SIGCHLD handler uses
 * this list to decide whether to print a "[Done]" notification — we only
 * want that message for genuine background jobs, not for the foreground
 * pipeline children that exec_pipeline has already waited for.
 */
#define MAX_BG_JOBS 64
static pid_t bg_pids[MAX_BG_JOBS];
static int   bg_count = 0;

void bg_register(pid_t pid)
{
    if (bg_count < MAX_BG_JOBS)
        bg_pids[bg_count++] = pid;
}

/* Returns 1 and removes the entry if pid is a registered background job. */
static int bg_remove(pid_t pid)
{
    for (int i = 0; i < bg_count; ++i) {
        if (bg_pids[i] == pid) {
            bg_pids[i] = bg_pids[--bg_count];
            return 1;
        }
    }
    return 0;
}

/* ── SIGCHLD handler — reap background zombies ───────────────────────── */
/*
 * Calls waitpid() with WNOHANG in a loop to collect all children that
 * have already exited.  Prints a notification only for registered
 * background jobs so that pipeline-stage exits are silent.
 */
static void sigchld_handler(int sig)
{
    (void)sig;

    pid_t pid;
    int   status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (bg_remove(pid)) {
            /* Only announce genuine background jobs. */
            char msg[64];
            int  len = snprintf(msg, sizeof msg,
                                "\r\n[Done] background process %d exited\r\n",
                                (int)pid);
            if (len > 0) {
                ssize_t r = write(STDOUT_FILENO, msg, (size_t)len);
                (void)r;
            }
        }
        /* Otherwise: foreground pipeline child already reaped — just discard. */
    }
}

/* ── prompt builder ──────────────────────────────────────────────────── */
/*
 * Builds a coloured prompt of the form:
 *
 *   username@hostname:~/path/to/cwd myshell>
 *
 * Falls back gracefully if any component is unavailable.
 */
static void print_prompt(void)
{
    /* Username */
    char *user = getenv("USER");
    if (!user) user = "user";

    /* Hostname (truncated to first dot for brevity) */
    char host[256] = "localhost";
    gethostname(host, sizeof host - 1);
    char *dot = strchr(host, '.');
    if (dot) *dot = '\0';

    /* Working directory — shorten $HOME to ~ */
    char cwd[PATH_MAX] = "?";
    if (getcwd(cwd, sizeof cwd) == NULL)
        strncpy(cwd, "?", sizeof cwd);

    char *home   = getenv("HOME");
    char  display_cwd[PATH_MAX];
    if (home && strncmp(cwd, home, strlen(home)) == 0) {
        snprintf(display_cwd, sizeof display_cwd, "~%s", cwd + strlen(home));
    } else {
        snprintf(display_cwd, sizeof display_cwd, "%s", cwd);
    }

    printf(C_GREEN "%s@%s" C_RESET ":" C_BLUE "%s" C_RESET
           " " C_BOLD C_CYAN SHELL_NAME ">" C_RESET " ",
           user, host, display_cwd);

    fflush(stdout);
}

/* ── welcome banner ──────────────────────────────────────────────────── */
static void print_banner(void)
{
    printf("\n");
    printf(C_BOLD C_CYAN
        "  ╔══════════════════════════════════════════════════╗\n"
        "  ║         myshell  —  a custom Linux shell         ║\n"
        "  ║                                                  ║\n"
        "  ║  Type 'help' for built-in commands.              ║\n"
        "  ║  Type 'exit' or press Ctrl-D to quit.            ║\n"
        "  ╚══════════════════════════════════════════════════╝\n"
        C_RESET "\n");
}

/* ── public lifecycle ────────────────────────────────────────────────── */

void shell_init(void)
{
    /* Initialise history ring buffer to NULL. */
    memset(history, 0, sizeof history);

    /* ── signal handlers ── */

    struct sigaction sa_ign, sa_chld;

    /* Shell ignores SIGINT and SIGQUIT; children will override. */
    sa_ign.sa_handler = SIG_IGN;
    sigemptyset(&sa_ign.sa_mask);
    sa_ign.sa_flags = 0;
    sigaction(SIGINT,  &sa_ign, NULL);
    sigaction(SIGQUIT, &sa_ign, NULL);
    sigaction(SIGTSTP, &sa_ign, NULL);

    /* SIGCHLD: reap background children automatically. */
    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART;   /* restart interrupted read() */
    sigaction(SIGCHLD, &sa_chld, NULL);

    print_banner();
}

void shell_loop(void)
{
    char  line[SHELL_MAX_INPUT];
    int   last_status = 0;

    while (1) {
        print_prompt();

        /* ── read one line of input ── */
        if (fgets(line, sizeof line, stdin) == NULL) {
            /*
             * EOF (Ctrl-D) or read error.
             *   • EOF on a terminal → polite logout message.
             *   • EOF on a script   → normal termination.
             */
            if (feof(stdin)) {
                printf("\n" C_YELLOW "logout" C_RESET "\n");
                shell_exit(last_status);
            }

            /*
             * EINTR means a signal handler ran and interrupted fgets.
             * This can happen on SIGCHLD.  Simply retry.
             */
            if (errno == EINTR) {
                clearerr(stdin);
                continue;
            }

            perror(SHELL_NAME ": fgets");
            shell_exit(1);
        }

        /* Strip the trailing newline. */
        line[strcspn(line, "\n")] = '\0';

        /* Skip empty lines; do not push them to history. */
        if (line[0] == '\0') continue;

        /* Push to history before execution (bash-compatible). */
        history_push(line);

        /* ── parse ── */
        Pipeline *pl = parse_input(line);
        if (!pl) continue;   /* blank, comment, or parse error */

        /* ── execute ── */
        last_status = executor_run(pl);

        /* ── clean up ── */
        pipeline_free(pl);
    }
    /* Not reached. */
}

void shell_cleanup(void)
{
    for (int i = 0; i < MAX_HISTORY; ++i) {
        free(history[i]);
        history[i] = NULL;
    }
}

void shell_exit(int code)
{
    shell_cleanup();
    printf(C_YELLOW "Goodbye!\n" C_RESET);
    exit(code);
}
