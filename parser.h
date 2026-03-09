/*
 * parser.h — Command-line parsing interface
 *
 * The parser converts a raw input string into a Pipeline — an ordered
 * sequence of Commands separated by '|'.  Each Command carries its
 * argument vector plus any I/O redirection metadata.
 *
 * Ownership:  parse_input() heap-allocates the Pipeline and all strings
 * inside it.  The caller must call pipeline_free() when done.
 */

#ifndef PARSER_H
#define PARSER_H

/* ── compile-time tunables ───────────────────────────────────────────── */
#define MAX_ARGS        128     /* tokens per single command              */
#define MAX_COMMANDS    32      /* stages in one pipeline                 */
#define MAX_REDIRECTS   8       /* redirections per command (reserved)    */

/* ────────────────────────────────────────────────────────────────────── */

/*
 * Command — one stage in a pipeline.
 *
 * args[]      NULL-terminated argv suitable for passing to execvp().
 * input_file  path for '<'  redirection, NULL if absent.
 * output_file path for '>' / '>>' redirection, NULL if absent.
 * append      1 = open output_file in append mode (>>), 0 = truncate (>).
 * background  1 = run the whole pipeline in background ('&' suffix).
 *             Only the last Command in a Pipeline has this flag set;
 *             the executor reads it from Pipeline.background instead.
 */
typedef struct {
    char *args[MAX_ARGS];   /* argv[0] is the program name              */
    char *input_file;
    char *output_file;
    int   append;
} Command;

/*
 * Pipeline — the result of parsing one input line.
 *
 * commands[]  array of Command structs in pipe order.
 * count       number of populated entries in commands[].
 * background  1 if the pipeline should run in the background ('&').
 */
typedef struct {
    Command commands[MAX_COMMANDS];
    int     count;
    int     background;
} Pipeline;

/* ── public API ──────────────────────────────────────────────────────── */

/*
 * parse_input – tokenise and classify one input line.
 *
 * Returns a heap-allocated Pipeline on success, or NULL if the line
 * is empty / contains only whitespace or comments.
 * The caller owns the returned object and must call pipeline_free().
 */
Pipeline *parse_input(char *line);

/*
 * pipeline_free – release all memory associated with a Pipeline.
 * Passing NULL is a no-op.
 */
void pipeline_free(Pipeline *pl);

/*
 * pipeline_print – dump a Pipeline to stdout (for debugging).
 */
void pipeline_print(const Pipeline *pl);

#endif /* PARSER_H */
