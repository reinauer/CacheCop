CC = m68k-amigaos-gcc
CFLAGS = -O2 -noixemul -Wall -Wextra -Werror \
  -fno-builtin -fomit-frame-pointer
EXTRA_CFLAGS ?=

# Point this to your NDK include_h directory if not in default search path
# Example: NDK=/opt/amiga/NDK/3.2/Include/include_h
NDK ?= /opt/amiga/m68k-amigaos/ndk-include

ifneq ($(NDK),)
  CFLAGS += -I$(NDK)
endif

SRC := cachecop.c
BIN := cachecop

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ $^

clean:
	rm -f $(BIN)

.PHONY: all clean
