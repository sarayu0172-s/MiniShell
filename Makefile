# ─────────────────────────────────────────────────────────────────────────────
# Makefile — myshell build system
#
# Targets:
#   make           → build the shell (default)
#   make debug     → build with AddressSanitizer + full debug info
#   make clean     → remove build artefacts
#   make valgrind  → run under valgrind (requires valgrind to be installed)
#   make test      → run a smoke-test script
# ─────────────────────────────────────────────────────────────────────────────

CC      := gcc

# ── Release flags ────────────────────────────────────────────────────────────
CFLAGS  := -Wall -Wextra -Wpedantic \
            -Wshadow -Wformat=2 -Wstrict-prototypes \
            -std=c11 \
            -O2

# ── Debug / sanitizer flags ──────────────────────────────────────────────────
DBGFLAGS := -Wall -Wextra -Wpedantic \
             -std=c11 \
             -g3 -O0 \
             -fsanitize=address,undefined \
             -fno-omit-frame-pointer

TARGET  := myshell

SRCS    := main.c \
            shell.c \
            parser.c \
            builtins.c \
            executor.c

OBJS    := $(SRCS:.c=.o)

# ─────────────────────────────────────────────────────────────────────────────
# Default target
# ─────────────────────────────────────────────────────────────────────────────
.PHONY: all
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^
	@echo ""
	@echo "  ✓  Built $(TARGET)  —  run with  ./$(TARGET)"
	@echo ""

# Pattern rule: compile each .c → .o
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Header dependencies (keep make from skipping a rebuild when headers change)
main.o:     main.c     shell.h
shell.o:    shell.c    shell.h parser.h executor.h
parser.o:   parser.c   parser.h shell.h
builtins.o: builtins.c builtins.h shell.h parser.h
executor.o: executor.c executor.h parser.h builtins.h shell.h

# ─────────────────────────────────────────────────────────────────────────────
# Debug build (separate directory to avoid mixing with release objects)
# ─────────────────────────────────────────────────────────────────────────────
.PHONY: debug
debug: CFLAGS := $(DBGFLAGS)
debug: clean $(TARGET)
	@echo "  ✓  Debug build complete."

# ─────────────────────────────────────────────────────────────────────────────
# Clean
# ─────────────────────────────────────────────────────────────────────────────
.PHONY: clean
clean:
	rm -f $(OBJS) $(TARGET)
	@echo "  ✓  Cleaned."

# ─────────────────────────────────────────────────────────────────────────────
# Valgrind (memory leak check)
# ─────────────────────────────────────────────────────────────────────────────
.PHONY: valgrind
valgrind: $(TARGET)
	valgrind --leak-check=full \
	         --show-leak-kinds=all \
	         --track-origins=yes \
	         --error-exitcode=1 \
	         ./$(TARGET)

# ─────────────────────────────────────────────────────────────────────────────
# Smoke test (non-interactive; checks exit codes)
# ─────────────────────────────────────────────────────────────────────────────
.PHONY: test
test: $(TARGET)
	@echo "Running smoke tests..."
	@echo "pwd"          | ./$(TARGET) > /dev/null && echo "  [PASS] pwd"
	@echo "ls -la /tmp"  | ./$(TARGET) > /dev/null && echo "  [PASS] ls"
	@echo "echo hello"   | ./$(TARGET) | grep -q "hello" && echo "  [PASS] echo"
	@echo "cd /tmp && pwd" | ./$(TARGET) | grep -q "/tmp" && echo "  [PASS] cd"
	@printf 'echo a\necho b\nhistory\n' | ./$(TARGET) | grep -q "echo a" && echo "  [PASS] history"
	@echo "  All smoke tests passed."
