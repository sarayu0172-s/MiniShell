/*
 * builtins.h — Built-in command interface
 *
 * Built-in commands are handled directly by the shell process; they do not
 * fork a child.  This is required for commands that must affect the shell's
 * own state (cd, exit) and desirable for lightweight utilities (pwd, help,
 * history) to avoid fork overhead.
 *
 * Each builtin is represented by a (name, handler) pair.  The executor
 * checks this table before attempting fork/exec.
 */

#ifndef BUILTINS_H
#define BUILTINS_H

#include "parser.h"

/* ── handler signature ───────────────────────────────────────────────── */
/*
 * A builtin handler receives the fully-parsed Command and returns an
 * exit-status integer (0 = success, non-zero = failure) identical to
 * what a child process would return.
 */
typedef int (*builtin_fn)(const Command *cmd);

/* ── registry entry ──────────────────────────────────────────────────── */
typedef struct {
    const char *name;
    builtin_fn  handler;
    const char *usage;          /* one-line usage shown by 'help'        */
    const char *description;    /* short description shown by 'help'     */
} BuiltinEntry;

/* ── public API ──────────────────────────────────────────────────────── */

/*
 * builtins_lookup – search the registry for a command name.
 * Returns a pointer to the matching BuiltinEntry, or NULL if not found.
 */
const BuiltinEntry *builtins_lookup(const char *name);

/*
 * builtins_exec – run a built-in if the command name matches one.
 * Returns the exit status, or -1 if the command is NOT a built-in
 * (caller should then fork/exec it).
 */
int builtins_exec(const Command *cmd);

/*
 * builtins_list – return the NULL-terminated array of all entries.
 * Useful for implementing tab-completion or 'help' listings.
 */
const BuiltinEntry *builtins_list(void);

/* ── individual handlers (exposed for unit testing) ─────────────────── */
int builtin_cd(const Command *cmd);
int builtin_exit(const Command *cmd);
int builtin_help(const Command *cmd);
int builtin_pwd(const Command *cmd);
int builtin_history(const Command *cmd);

#endif /* BUILTINS_H */
