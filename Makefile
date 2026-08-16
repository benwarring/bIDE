CC = gcc
# -MMD -MP emit a .d file per object listing the headers it included, so editing
# a header rebuilds everything that includes it
CFLAGS = -Wall -Wextra -pedantic -std=c17 -g -D_DEFAULT_SOURCE -I$(INCLUDE) -MMD -MP

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
	$(CC) $(CFLAGS) -c $< -o $@

-include $(DEPS)

.PHONY: all clean

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)