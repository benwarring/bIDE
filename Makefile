CC = gcc
# -MMD -MP emit a .d file per object listing the headers it included, so editing
# a header rebuilds everything that includes it
CFLAGS = -Wall -Wextra -pedantic -std=c17 -g -D_DEFAULT_SOURCE -I$(INCLUDE) -MMD -MP
# where the checkout is, so bin/main finds assets/ when launched from anywhere.
# Kept out of CFLAGS so `make asan` (which replaces CFLAGS) still gets it.
CPPFLAGS = -DBIDE_ROOT='"$(CURDIR)"'

SRC = src
OBJ = obj
BIN = bin
INCLUDE = include

SRCS = $(wildcard $(SRC)/*.c)
OBJS = $(patsubst $(SRC)/%.c,$(OBJ)/%.o,$(SRCS))
DEPS = $(OBJS:.o=.d)
TARGET = $(BIN)/main

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ -o $@

$(OBJ)/%.o: $(SRC)/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

-include $(DEPS)

# clangd reads compile_commands.json to learn the include paths and flags. Without
# it, it guesses, fails to resolve our own headers, and reports an error on every
# #include — which looks exactly like a broken LSP integration and is not one.
# `bear -- make` is the usual generator; this target avoids the dependency, since
# every object here is built with the same command.
compile_commands.json: Makefile
	@printf '[\n' > $@
	@sep=""; for f in $(SRCS); do \
		printf '%s  {"directory": "%s", "file": "%s", "command": "$(CC) $(CFLAGS) -c %s -o $(OBJ)/%s.o"}\n' \
			"$$sep" "$(CURDIR)" "$$f" "$$f" "$$(basename $$f .c)" >> $@; \
		sep=","; \
	done
	@printf ']\n' >> $@
	@echo "wrote $@ ($(words $(SRCS)) entries)"

SANFLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer

# A sanitized build, for the class of bug this project only started generating
# once it grew an async subsystem: a reply handler touching a buffer the user has
# since closed, or a popup left borrowing strings that have been freed. Neither
# shows up in a normal build until it corrupts something unrelated.
#
# Recursive rather than a target-specific variable because the pattern rule bakes
# in the object directory at parse time; this way the flags reach every object.
asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) $(SANFLAGS)" all

.PHONY: all clean compdb asan

compdb: compile_commands.json

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)