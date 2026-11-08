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

SRC := cachecop.c stubs.s
BIN := cachecop

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ $^

# Test program and a data floppy holding both binaries
tests/dmatest: tests/dmatest.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -Wno-pointer-sign -Wno-discarded-qualifiers -o $@ $^

test-adf: $(BIN) tests/dmatest
	rm -f cachecop-test.adf
	xdftool cachecop-test.adf create + format CacheCop \
		+ write $(BIN) + write tests/dmatest \
		+ makedir S + write tests/Startup-Sequence S/Startup-Sequence \
		+ boot install

clean:
	rm -f $(BIN) tests/dmatest cachecop-test.adf

.PHONY: all clean test-adf
