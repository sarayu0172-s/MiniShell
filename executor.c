/*
 * executor.c — Pipeline execution engine
 *
 * This is the heart of the shell.  It receives a fully-parsed Pipeline and
 * carries out the following steps:
 *
 *   1. If the pipeline has exactly one command and it is a built-in, run it
 *      directly in the shell process (no fork needed).
 *
 *   2. Otherwise, for a pipeline of N commands:
 *        • Create N-1 pipe(2) pairs.
 *        • For each command (stage i):
 *            a. fork(2) a child process.
 *            b. In the child:
 *               - Restore default signal dispositions.
 *               - Wire stdin from the read-end of pipe[i-1]  (if i > 0).
 *               - Wire stdout to the write-end of pipe[i]   (if i < N-1).
 *               - Apply any explicit I/O redirections (< > >>).
 *               - Close all pipe file descriptors that this child does not use.
 *               - execvp(2) the external command.
 *            c. In the parent:
 *               - Close the write-end of the just-used pipe (important!).
 *        • For foreground pipelines: wait for every child with waitpid(2).
 *        • For background pipelines: print the PID and return immediately.
 *
 * Key system calls used
 * ─────────────────────
 *   fork(2)   — duplicate the process; child inherits open file descriptors.
 *   execvp(2) — replace the child image with the external program.
 *   pipe(2)   — create a unidirectional byte stream between processes.
 *   dup2(2)   — atomically duplicate an fd onto stdin/stdout slot.
 *   open(2)   — open redirection target files.
 *   waitpid(2)— parent blocks until a specific child exits.
 *   kill(2)   — send SIGTERM to misbehaving children (not used here, for
 *               future job-control extension).
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "executor.h"
#include "parser.h"
#include "builtins.h"
#include "shell.h"

/* ── internal: close all pipe fds in one go ──────────────────────────── */
/*
 * After forking, each child process inherits ALL pipe file descriptors
 * that were created before it was forked.  We must close the ones that
 * this child is not going to use, otherwise readers will never see EOF
 * (because the write-end is still open in the child).
 *
 * pipe_fds[i][0] = read  end of pipe i
 * pipe_fds[i][1] = write end of pipe i
 */
static void close_all_pipes(int pipe_fds[][2], int n_pipes, int keep_read, int keep_write)
{
    for (int i = 0; i < n_pipes; ++i) {
        if (pipe_fds[i][0] != keep_read)
            close(pipe_fds[i][0]);
        if (pipe_fds[i][1] != keep_write)
            close(pipe_fds[i][1]);
    }
}

/* ── internal: apply I/O redirections inside the child ───────────────── */
/*
 * Returns 0 on success, -1 on error (child should exit on error).
 */
static int apply_redirections(const Command *cmd)
{
    /* Input redirection: < filename */
    if (cmd->input_file) {
        int fd = open(cmd->input_file, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, SHELL_NAME ": %s: %s\n",
                    cmd->input_file, strerror(errno));
            return -1;
        }
        if (dup2(fd, STDIN_FILENO) < 0) {
            perror(SHELL_NAME ": dup2 (input)");
            close(fd);
            return -1;
        }
        close(fd);
    }

    /* Output redirection: > filename   or   >> filename */
    if (cmd->output_file) {
        int flags = O_WRONLY | O_CREAT | (cmd->append ? O_APPEND : O_TRUNC);
        int fd    = open(cmd->output_file, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, SHELL_NAME ": %s: %s\n",
                    cmd->output_file, strerror(errno));
            return -1;
        }
        if (dup2(fd, STDOUT_FILENO) < 0) {
            perror(SHELL_NAME ": dup2 (output)");
            close(fd);
            return -1;
        }
        close(fd);
    }

    return 0;
}

/* ── internal: reset signal handlers in the child ─────────────────────── */
/*
 * The shell ignores SIGINT so that Ctrl-C kills the foreground process but
 * not the shell itself.  Each forked child must restore the default so that
 * it can be killed normally.
 */
static void child_reset_signals(void)
{
    signal(SIGINT,  SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
}

/* ── internal: execute a single (no-pipe) command ─────────────────────── */
/*
 * Tries built-ins first.  If not a built-in, forks and execs.
 * Returns the child exit status (or built-in return value).
 */
static int exec_single(Pipeline *pl)
{
    Command *cmd = &pl->commands[0];

    /* ── built-in? ── */
    int rc = builtins_exec(cmd);
    if (rc != -1)
        return rc;

    /* ── external command ── */
    pid_t pid = fork();

    if (pid < 0) {
        perror(SHELL_NAME ": fork");
        return 1;
    }

    if (pid == 0) {
        /* ---- child ---- */
        child_reset_signals();

        if (pl->background) {
            /* Detach from terminal's process group so Ctrl-C won't reach it. */
            setpgid(0, 0);
        }

        if (apply_redirections(cmd) < 0)
            exit(1);

        execvp(cmd->args[0], cmd->args);

        /* execvp only returns on error. */
        fprintf(stderr, SHELL_NAME ": %s: %s\n",
                cmd->args[0], strerror(errno));
        exit(127);   /* 127 is the conventional "command not found" code */
    }

    /* ---- parent ---- */
    if (pl->background) {
        bg_register(pid);
        printf("[background] pid %d\n", (int)pid);
        return 0;
    }

    /* Wait for the foreground child.
     * Loop on EINTR (signal handler interrupted the syscall).
     * Handle ECHILD gracefully: the SIGCHLD handler may have already
     * reaped this child; treat that as exit status 0.               */
    int status = 0;
    pid_t r;
    do {
        r = waitpid(pid, &status, 0);
    } while (r == (pid_t)-1 && errno == EINTR);

    if (r == (pid_t)-1) {
        if (errno != ECHILD)
            perror(SHELL_NAME ": waitpid");
        return 0;   /* already reaped by SIGCHLD handler */
    }

    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        if (sig != SIGINT)
            fprintf(stderr, "\n" SHELL_NAME ": process killed by signal %d\n", sig);
        else
            putchar('\n');
        return 128 + sig;
    }

    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* ── internal: execute a multi-stage pipeline ─────────────────────────── */
/*
 * Algorithm:
 *   Allocate (count-1) pipes.
 *   Fork each stage; wire its stdin/stdout to adjacent pipe ends.
 *   Parent closes write-ends progressively so children see EOF.
 *   Wait for all children (or detach if background).
 */
static int exec_pipeline(Pipeline *pl)
{
    int n          = pl->count;
    int n_pipes    = n - 1;

    /* Dynamically allocate the pipe array. */
    int (*pipe_fds)[2] = malloc(n_pipes * sizeof(int[2]));
    if (!pipe_fds) { perror("malloc"); return 1; }

    /* Create all pipes up-front. */
    for (int i = 0; i < n_pipes; ++i) {
        if (pipe(pipe_fds[i]) < 0) {
            perror(SHELL_NAME ": pipe");
            /* Close already-opened pipes before returning. */
            for (int j = 0; j < i; ++j) {
                close(pipe_fds[j][0]);
                close(pipe_fds[j][1]);
            }
            free(pipe_fds);
            return 1;
        }
    }

    if (n <= 0 || n > MAX_COMMANDS) { free(pipe_fds); return 1; }
    pid_t *pids = malloc((size_t)n * sizeof(pid_t));
    if (!pids) {
        perror("malloc");
        for (int i = 0; i < n_pipes; ++i) {
            close(pipe_fds[i][0]);
            close(pipe_fds[i][1]);
        }
        free(pipe_fds);
        return 1;
    }

    for (int i = 0; i < n; ++i) {
        Command *cmd = &pl->commands[i];

        pid_t pid = fork();
        if (pid < 0) {
            perror(SHELL_NAME ": fork");
            /* Kill already-forked children before returning. */
            for (int j = 0; j < i; ++j)
                kill(pids[j], SIGTERM);
            break;
        }

        if (pid == 0) {
            /* ==== child ==== */
            child_reset_signals();

            if (pl->background)
                setpgid(0, 0);

            /* Wire stdin from the previous pipe's read-end (if not first). */
            if (i > 0) {
                if (dup2(pipe_fds[i - 1][0], STDIN_FILENO) < 0) {
                    perror(SHELL_NAME ": dup2 (stdin pipe)");
                    exit(1);
                }
            }

            /* Wire stdout to the next pipe's write-end (if not last). */
            if (i < n_pipes) {
                if (dup2(pipe_fds[i][1], STDOUT_FILENO) < 0) {
                    perror(SHELL_NAME ": dup2 (stdout pipe)");
                    exit(1);
                }
            }

            /* Close ALL pipe fds; we've duped the ones we need. */
            close_all_pipes(pipe_fds, n_pipes, -1, -1);

            /* Apply file-based redirections (override pipe if present). */
            if (apply_redirections(cmd) < 0)
                exit(1);

            execvp(cmd->args[0], cmd->args);
            fprintf(stderr, SHELL_NAME ": %s: %s\n",
                    cmd->args[0], strerror(errno));
            exit(127);
        }

        /* ==== parent: store PID, close write-end of the pipe we just used ==== */
        pids[i] = pid;

        /*
         * Close the write-end of pipe[i-1] in the parent.
         * This is CRITICAL: if the parent keeps it open, the reader
         * (stage i's stdin) will never see EOF even after the writer exits.
         */
        if (i > 0)
            close(pipe_fds[i - 1][0]);
        if (i < n_pipes)
            close(pipe_fds[i][1]);
    }

    /* Close any remaining pipe ends in the parent. */
    for (int i = 0; i < n_pipes; ++i) {
        /* These may already be closed, but close(-1) is checked in
         * close_all_pipes only; here we just close them if still open.
         * Using the raw close() call is safe; duplicate closes on the
         * same fd return EBADF which we can safely ignore.              */
        close(pipe_fds[i][0]);
        close(pipe_fds[i][1]);
    }

    free(pipe_fds);

    /* ── wait for all children (foreground) ── */
    int last_status = 0;

    if (pl->background) {
        printf("[background pipeline] pids:");
        for (int i = 0; i < n; ++i) {
            bg_register(pids[i]);
            printf(" %d", (int)pids[i]);
        }
        putchar('\n');
    } else {
        for (int i = 0; i < n; ++i) {
            int status;
            pid_t wr;
            do {
                wr = waitpid(pids[i], &status, 0);
            } while (wr == (pid_t)-1 && errno == EINTR);

            if (wr == (pid_t)-1) {
                /* ECHILD: already reaped by SIGCHLD handler; treat as ok */
                if (errno != ECHILD)
                    perror(SHELL_NAME ": waitpid");
                continue;
            }

            /* We only care about the last (rightmost) stage's exit code,
             * which is what bash does.                                   */
            if (i == n - 1) {
                if (WIFSIGNALED(status)) {
                    int sig = WTERMSIG(status);
                    if (sig == SIGINT) putchar('\n');
                    else fprintf(stderr, "\n" SHELL_NAME ": killed by signal %d\n", sig);
                    last_status = 128 + sig;
                } else if (WIFEXITED(status)) {
                    last_status = WEXITSTATUS(status);
                }
            }
        }
    }

    free(pids);
    return last_status;
}

/* ── public API ──────────────────────────────────────────────────────── */

int executor_run(Pipeline *pl)
{
    if (!pl || pl->count == 0) return 0;

    if (pl->count == 1)
        return exec_single(pl);
    else
        return exec_pipeline(pl);
}
