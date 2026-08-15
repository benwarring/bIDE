CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c17 -g -D_DEFAULT_SOURCE -I$(INCLUDE)

SRC = src
OBJ = obj
BIN = bin
INCLUDE = include

SRCS = $(wildcard $(SRC)/*.c)
OBJS = $(patsubst $(SRC)/%.c,$(OBJ)/%.o,$(SRCS))
TARGET = $(BIN)/main

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ -o $@

$(OBJ)/%.o: $(SRC)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: all clean

clean:
	rm -f $(OBJS) $(TARGET)