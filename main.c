/*
 * main.c — Shell entry point
 *
 * The entry point is intentionally minimal.  All real work is delegated to
 * shell_init()  (one-time setup)
 * shell_loop()  (the REPL — never returns)
 *
 * This separation means each subsystem (parser, executor, builtins) can be
 * compiled and unit-tested independently without dragging in main().
 *
 * Compile:  make
 * Run:      ./myshell
 */

#include <stdio.h>
#include "shell.h"

int main(void)
{
    shell_init();
    shell_loop();   /* does not return */
    return 0;       /* unreachable; silences -Wreturn-type */
}
