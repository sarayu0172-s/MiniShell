# myshell — A Custom Linux Shell in C

> A university OS project demonstrating core POSIX concepts:
> process creation, pipes, I/O redirection, signal handling, and job control.

---

## Table of Contents

1. [How a Shell Works](#how-a-shell-works)
2. [System Calls Used](#system-calls-used)
3. [Architecture](#architecture)
4. [File Structure](#file-structure)
5. [Build & Run](#build--run)
6. [Feature Reference](#feature-reference)
7. [Sample Session](#sample-session)
8. [Edge Cases Handled](#edge-cases-handled)
9. [Future Improvements](#future-improvements)

---

## How a Shell Works

A Unix shell is a **command interpreter** that sits between the user and the
operating system kernel.  Its job, repeated in an infinite loop, is:

```
Read  →  Parse  →  Execute  →  Wait  →  (repeat)
```

This cycle is called the **REPL** (Read-Eval-Print Loop).

1. **Read** — The shell prints a prompt and reads one line of text from
   standard input using `fgets(3)`.

2. **Parse** — The raw string is split on pipe characters (`|`) to identify
   *pipeline stages*, and each stage is further tokenised to separate the
   program name, arguments, and any I/O redirections.

3. **Execute** — The shell calls `fork(2)` to clone itself.  The child
   calls `execvp(2)` to replace its image with the target program.
   If there are pipes, several children are created and connected via
   anonymous byte streams created by `pipe(2)`.

4. **Wait** — For foreground commands the parent calls `waitpid(2)` and
   blocks until the child exits.  For background commands (`&`) it moves
   straight back to the prompt; orphaned children are reaped automatically
   via a `SIGCHLD` handler.

---

## System Calls Used

| System call | Header         | Why we use it |
|-------------|----------------|---------------|
| `fork(2)`   | `<unistd.h>`   | Create a child process to run an external command without replacing the shell itself. |
| `execvp(2)` | `<unistd.h>`   | Replace the child's memory image with the target program.  The `p` variant searches `$PATH`. |
| `waitpid(2)`| `<sys/wait.h>` | Block the parent until a specific child exits; retrieve its exit status. |
| `pipe(2)`   | `<unistd.h>`   | Create a unidirectional byte channel between two processes (the kernel buffer is ~64 KB). |
| `dup2(2)`   | `<unistd.h>`   | Atomically copy an open file descriptor onto stdin (fd 0) or stdout (fd 1), enabling transparent piping and redirection. |
| `open(2)`   | `<fcntl.h>`    | Open files for `<`, `>`, and `>>` redirections. |
| `chdir(2)`  | `<unistd.h>`   | Change the shell process's working directory (`cd` built-in). |
| `getcwd(2)` | `<unistd.h>`   | Read the current working directory for the prompt and `pwd`. |
| `sigaction(2)` | `<signal.h>` | Install signal handlers with better semantics than `signal(2)`. |
| `kill(2)`   | `<signal.h>`   | Send signals to child processes (used for clean-up on error). |
| `setpgid(2)`| `<unistd.h>`   | Move background processes to their own process group so `Ctrl-C` does not reach them. |

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                         myshell                             │
│                                                             │
│  ┌──────────┐    ┌──────────┐    ┌───────────────────────┐  │
│  │  shell.c │───▶│ parser.c │───▶│     executor.c        │  │
│  │  (REPL)  │    │ (lexer + │    │  ┌──────────────────┐ │  │
│  │          │    │  parser) │    │  │   builtins.c     │ │  │
│  │ history  │    └──────────┘    │  │  cd/exit/pwd/    │ │  │
│  │ signals  │                   │  │  help/history    │ │  │
│  └──────────┘                   │  └──────────────────┘ │  │
│                                 │  fork → execvp         │  │
│                                 │  pipe + dup2           │  │
│                                 │  waitpid               │  │
│                                 └───────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

**Data flow for `ls -la | grep foo > out.txt`:**

```
  Input string: "ls -la | grep foo > out.txt"
       │
       ▼
  parser.c: split on '|'
       │
       ├── Stage 0: args=["ls","-la"]       input=NULL  output=NULL
       └── Stage 1: args=["grep","foo"]     input=NULL  output="out.txt"
       │
       ▼
  executor.c:
    pipe(fd0)                  ← kernel creates byte channel
    fork child 0: ls -la       → writes to fd0[1]
    fork child 1: grep foo     ← reads from fd0[0], writes to out.txt
    parent closes fd0[1]       ← critical: lets reader see EOF
    waitpid(child0)
    waitpid(child1)
```

---

## File Structure

```
myshell/
├── main.c          Entry point; calls shell_init() + shell_loop()
├── shell.h         Shared constants, ANSI colours, history API
├── shell.c         REPL, prompt, signal handlers, history ring buffer
├── parser.h        Pipeline / Command data structures
├── parser.c        Tokeniser: splits pipes, redirections, background
├── builtins.h      Built-in registry type definitions
├── builtins.c      cd, exit, pwd, help, history implementations
├── executor.h      executor_run() declaration
├── executor.c      fork/exec, pipe wiring, dup2, waitpid
└── Makefile        Build, debug, clean, test targets
```

---

## Build & Run

### Requirements

- Linux (tested on Ubuntu 20.04+ and Debian 11+)
- GCC 9+ or Clang 11+
- GNU Make

### Build

```bash
# Clone / copy files into a directory, then:
cd myshell
make
```

### Run

```bash
./myshell
```

### Debug build (AddressSanitizer + UBSan)

```bash
make debug
./myshell
```

### Check for memory leaks

```bash
make valgrind
```

### Smoke tests

```bash
make test
```

---

## Feature Reference

### Built-in Commands

| Command         | Description |
|----------------|-------------|
| `cd [dir\|-]`  | Change directory; `cd -` goes to previous dir |
| `exit [n]`     | Exit with optional status code |
| `pwd`          | Print current working directory |
| `help`         | Display built-in command reference |
| `history [n]`  | Show last *n* commands (default: all) |

### I/O Redirection

```bash
cmd < input.txt          # read stdin from file
cmd > output.txt         # write stdout to file (truncate)
cmd >> output.txt        # write stdout to file (append)
cmd < in.txt > out.txt   # both at once
```

### Pipes

```bash
cmd1 | cmd2                     # single pipe
cmd1 | cmd2 | cmd3 | ...        # arbitrary-length pipeline
cat file | grep pattern | sort  # classic example
```

### Background Execution

```bash
long_running_command &          # shell returns immediately
sleep 10 &                      # background sleep
```

### Signal Handling

| Signal | Behaviour |
|--------|-----------|
| `Ctrl-C` (SIGINT)  | Kills the *foreground child*; shell survives |
| `Ctrl-\` (SIGQUIT) | Ignored by the shell |
| `Ctrl-Z` (SIGTSTP) | Ignored (no job control yet) |
| `Ctrl-D` (EOF)     | Clean shell exit |
| SIGCHLD            | Auto-reaps background children (no zombies) |

---

## Sample Session

```
  ╔══════════════════════════════════════════════════╗
  ║         myshell  —  a custom Linux shell         ║
  ╚══════════════════════════════════════════════════╝

user@host:~ myshell> pwd
/home/user

user@host:~ myshell> echo "hello world"
hello world

user@host:~ myshell> ls | grep ".c" | sort
builtins.c
executor.c
main.c
parser.c
shell.c

user@host:~ myshell> cat /etc/os-release | grep NAME | head -1
NAME="Ubuntu"

user@host:~ myshell> echo "line1" > /tmp/demo.txt
user@host:~ myshell> echo "line2" >> /tmp/demo.txt
user@host:~ myshell> cat /tmp/demo.txt
line1
line2

user@host:~ myshell> sort < /tmp/demo.txt
line1
line2

user@host:~ myshell> sleep 5 &
[background] pid 12345

user@host:~ myshell> history
     1  pwd
     2  echo "hello world"
     ...

user@host:~ myshell> cd /tmp
user@host:/tmp myshell> cd -
/home/user
user@host:~ myshell> exit
Goodbye!
```

---

## Edge Cases Handled

| Scenario | Behaviour |
|----------|-----------|
| Empty input | Silently re-prompts |
| Comment lines (`#`) | Silently skipped |
| Unknown command | Prints `command: No such file or directory`, status 127 |
| `cd` with no `$HOME` | Prints an informative error |
| `cd -` with no `$OLDPWD` | Prints an informative error |
| Pipe with empty stage (`ls \|`) | Parse error message |
| Redirection without filename | Parse error message |
| `>` to read-only path | `open(2)` error is reported |
| Child killed by signal | Shell prints newline, continues |
| `exit` with non-numeric arg | Error message, shell continues |
| History wraps at 500 entries | Ring buffer discards oldest |
| Background child exits | SIGCHLD handler prints notification |

---

## Future Improvements

| Feature | Notes |
|---------|-------|
| **Job control** (`fg`, `bg`, `jobs`) | Requires `tcsetpgrp` and proper SIGTSTP handling |
| **Tab completion** | Use `readline` or `libedit` |
| **Command history persistence** | Write history to `~/.myshell_history` on exit |
| **Environment variable expansion** | `$VAR` and `${VAR}` substitution in the parser |
| **Glob expansion** | Use `glob(3)` to expand `*`, `?`, `[...]` |
| **Here documents** | `cmd << EOF` |
| **Logical operators** | `cmd1 && cmd2`, `cmd1 \|\| cmd2` |
| **Subshell** | `(cmd1; cmd2)` |
| **Aliases** | User-defined shorthand commands |
| **`source` / `.`** | Execute commands from a file in the current shell |

---

*Written as a learning project to demonstrate core OS concepts taught in an
Operating Systems course.  All system calls are used directly; no shell
library wrappers are used.*
