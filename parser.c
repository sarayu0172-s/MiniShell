/*
 * parser.c — Command-line tokeniser and structural analyser
 *
 * Overview
 * --------
 * parse_input() converts a raw input string into a Pipeline struct.
 * The algorithm works in two passes:
 *
 *   Pass 1 – Split by '|' to identify individual command segments.
 *             We must be careful not to split on '|' that appears inside
 *             quoted strings (basic quote awareness).
 *
 *   Pass 2 – For each segment, tokenise by whitespace and classify each
 *             token as: argument, input redirect (<), output redirect (>),
 *             append redirect (>>), or background marker (&).
 *
 * Limitations (by design, for clarity):
 *   • No brace expansion or globbing (shell-level; left to the kernel/exec).
 *   • No variable substitution.
 *   • Single-level quoting only (handles spaces inside "double quotes").
 *   • '&' must appear as the very last token of the whole pipeline.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "shell.h"
#include "parser.h"

/* ── internal helpers ────────────────────────────────────────────────── */

/* Skip leading whitespace and return pointer to first non-space char. */
static char *ltrim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        ++s;
    return s;
}

/* Remove trailing whitespace in-place. */
static void rtrim(char *s)
{
    int len = (int)strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
}

/*
 * split_pipes – split 'line' on unquoted '|' characters.
 *
 * Fills 'segments' with pointers into a copy of the line (allocated here;
 * the caller must free segments_buf).  Returns the number of segments.
 */
static int split_pipes(char *line, char **segments, int max_seg,
                        char **segments_buf)
{
    /* Work on a private copy so we can insert NULs. */
    char *buf = strdup(line);
    if (!buf) {
        perror("strdup");
        return 0;
    }
    *segments_buf = buf;

    int   count  = 0;
    char *p      = buf;
    char *start  = buf;
    int   in_dq  = 0;   /* inside double-quote? */
    int   in_sq  = 0;   /* inside single-quote? */

    for (; *p; ++p) {
        if (*p == '"'  && !in_sq) { in_dq = !in_dq; continue; }
        if (*p == '\'' && !in_dq) { in_sq = !in_sq; continue; }

        if (*p == '|' && !in_dq && !in_sq) {
            if (count >= max_seg - 1) {
                fprintf(stderr, SHELL_NAME ": too many pipes (max %d)\n",
                        max_seg - 1);
                break;
            }
            *p = '\0';
            segments[count++] = start;
            start = p + 1;
        }
    }
    segments[count++] = start;
    return count;
}

/*
 * parse_command – tokenise one pipe-segment into a Command.
 *
 * Handles:
 *   token      → appended to args[]
 *   <  token   → input_file
 *   >  token   → output_file (truncate)
 *   >> token   → output_file (append)
 *   &           → sets *bg flag (only meaningful on last segment)
 *
 * Returns 1 on success, 0 on parse error.
 */
static int parse_command(char *seg, Command *cmd, int *bg)
{
    /* Zero the struct. */
    memset(cmd, 0, sizeof *cmd);

    int   argc = 0;
    char *p    = ltrim(seg);

    while (*p) {
        /* --- skip whitespace between tokens --- */
        if (isspace((unsigned char)*p)) { ++p; continue; }

        /* --- handle >> before > (order matters) --- */
        if (p[0] == '>' && p[1] == '>') {
            p += 2;
            p  = ltrim(p);
            if (!*p) {
                fprintf(stderr, SHELL_NAME ": syntax error: expected filename after '>>'\n");
                return 0;
            }
            /* collect the filename token */
            char *tok_start = p;
            while (*p && !isspace((unsigned char)*p)) ++p;
            if (*p) *p++ = '\0';
            cmd->output_file = strdup(tok_start);
            cmd->append      = 1;
            continue;
        }

        if (*p == '>') {
            ++p;
            p = ltrim(p);
            if (!*p) {
                fprintf(stderr, SHELL_NAME ": syntax error: expected filename after '>'\n");
                return 0;
            }
            char *tok_start = p;
            while (*p && !isspace((unsigned char)*p)) ++p;
            if (*p) *p++ = '\0';
            cmd->output_file = strdup(tok_start);
            cmd->append      = 0;
            continue;
        }

        if (*p == '<') {
            ++p;
            p = ltrim(p);
            if (!*p) {
                fprintf(stderr, SHELL_NAME ": syntax error: expected filename after '<'\n");
                return 0;
            }
            char *tok_start = p;
            while (*p && !isspace((unsigned char)*p)) ++p;
            if (*p) *p++ = '\0';
            cmd->input_file = strdup(tok_start);
            continue;
        }

        if (*p == '&') {
            *bg = 1;
            ++p;
            continue;
        }

        /* --- regular token (possibly double-quoted) --- */
        if (argc >= MAX_ARGS - 1) {
            fprintf(stderr, SHELL_NAME ": too many arguments (max %d)\n",
                    MAX_ARGS - 1);
            return 0;
        }

        char token_buf[SHELL_MAX_INPUT];
        int  ti = 0;

        while (*p && !isspace((unsigned char)*p)) {
            if (*p == '"') {
                /* consume until closing quote */
                ++p;
                while (*p && *p != '"') {
                    if (ti < (int)sizeof(token_buf) - 1)
                        token_buf[ti++] = *p;
                    ++p;
                }
                if (*p == '"') ++p;
            } else {
                if (ti < (int)sizeof(token_buf) - 1)
                    token_buf[ti++] = *p;
                ++p;
            }
        }
        token_buf[ti] = '\0';

        cmd->args[argc++] = strdup(token_buf);
    }

    cmd->args[argc] = NULL;   /* NULL-terminate argv */
    return 1;
}

/* ── public API ──────────────────────────────────────────────────────── */

Pipeline *parse_input(char *line)
{
    if (!line) return NULL;

    /* Strip leading/trailing whitespace and ignore blank/comment lines. */
    char *trimmed = ltrim(line);
    rtrim(trimmed);
    if (*trimmed == '\0' || *trimmed == '#')
        return NULL;

    Pipeline *pl = calloc(1, sizeof *pl);
    if (!pl) { perror("calloc"); return NULL; }

    /* Split by '|'. */
    char *segments[MAX_COMMANDS];
    char *seg_buf = NULL;
    int   nseg    = split_pipes(trimmed, segments, MAX_COMMANDS, &seg_buf);

    if (nseg == 0) {
        free(seg_buf);
        free(pl);
        return NULL;
    }

    int bg = 0;

    for (int i = 0; i < nseg; ++i) {
        if (!parse_command(segments[i], &pl->commands[i], &bg)) {
            /* Parse error in one segment — clean up and return NULL. */
            pipeline_free(pl);
            free(seg_buf);
            return NULL;
        }
        /* A command with no args is a parse error (empty pipe stage). */
        if (pl->commands[i].args[0] == NULL) {
            fprintf(stderr, SHELL_NAME ": syntax error: empty command in pipeline\n");
            pipeline_free(pl);
            free(seg_buf);
            return NULL;
        }
        pl->count++;
    }

    pl->background = bg;

    free(seg_buf);
    return pl;
}

void pipeline_free(Pipeline *pl)
{
    if (!pl) return;

    for (int i = 0; i < pl->count; ++i) {
        Command *cmd = &pl->commands[i];
        for (int j = 0; cmd->args[j]; ++j)
            free(cmd->args[j]);
        free(cmd->input_file);
        free(cmd->output_file);
    }
    free(pl);
}

void pipeline_print(const Pipeline *pl)
{
    if (!pl) { printf("<null pipeline>\n"); return; }

    printf("Pipeline (%d command(s), background=%d):\n",
           pl->count, pl->background);

    for (int i = 0; i < pl->count; ++i) {
        const Command *cmd = &pl->commands[i];
        printf("  [%d] ", i);
        for (int j = 0; cmd->args[j]; ++j)
            printf("'%s' ", cmd->args[j]);
        if (cmd->input_file)
            printf("< '%s' ", cmd->input_file);
        if (cmd->output_file)
            printf("%s '%s' ", cmd->append ? ">>" : ">", cmd->output_file);
        putchar('\n');
    }
}
