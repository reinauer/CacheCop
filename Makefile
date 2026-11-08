# CacheCop Makefile for GCC (m68k-amigaos)
#
#   make            build cachecop, dmatest, the release LHA and ADF
#   make test-adf   bootable floppy that runs the test cases
#   make NDK=...    use a different NDK include directory

CC    = m68k-amigaos-gcc
STRIP = m68k-amigaos-strip

# FULL_VERSION is 1.2 for a tagged release (tag v1.2), otherwise
# 1.2-3-gabcdef[-dirty]. Without any tag it falls back to 0.0-g<hash>.
FULL_VERSION ?= $(shell git describe --tags --dirty 2>/dev/null | sed -E 's/^v//')
ifeq ($(FULL_VERSION),)
FULL_VERSION := 0.0-g$(shell git describe --always --dirty 2>/dev/null || echo unknown)
endif
FULL_VERSION := $(FULL_VERSION)
ADATE        := $(shell date '+%-d.%-m.%Y')

# NDK include path, by default the one that comes with the compiler
NDK ?= $(shell realpath $$(dirname $$(which $(CC)))/../m68k-amigaos/ndk-include 2>/dev/null)

CFLAGS = -O2 -noixemul -Wall -Wextra -Werror \
  -fno-builtin -fomit-frame-pointer
ifneq ($(NDK),)
  CFLAGS += -I$(NDK)
endif
EXTRA_CFLAGS ?=

SRC := cachecop.c stubs.s
BIN := cachecop

all: $(BIN) tests/dmatest lha disk

# Rebuild when the version changes (new commit, tag or dirty tree)
VERSION_STAMP = build/version
$(VERSION_STAMP): FORCE
	@mkdir -p build
	@printf '%s %s\n' '$(FULL_VERSION)' '$(ADATE)' > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv -f $@.tmp $@

$(BIN): $(SRC) $(VERSION_STAMP)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) \
		-DCC_VERSION='"$(FULL_VERSION)"' -DCC_DATE='"$(ADATE)"' \
		-o $@ $(SRC)
	$(STRIP) $@

tests/dmatest: tests/dmatest.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -Wno-pointer-sign -Wno-discarded-qualifiers -o $@ $^
	$(STRIP) $@

# Release archive: CacheCop-<version>/ with the program, test tool and docs
LHA_NAME = cachecop-$(FULL_VERSION).lha
# Amiga file names are limited to 30 characters (including .info)
LHA_DIR  = CacheCop-$(firstword $(subst -, ,$(FULL_VERSION)))$(if $(findstring -g,$(FULL_VERSION)),dev)
LHA_OPTS := $(shell if lha 2>&1 | grep 'archive-kanji-code' | grep -q 'latin1'; then echo '--system-kanji-code=utf8 --archive-kanji-code=latin1'; fi)

lha: $(LHA_NAME)
$(LHA_NAME): $(BIN) tests/dmatest cachecop.readme README.md
	rm -rf build/$(LHA_DIR)
	mkdir -p build/$(LHA_DIR)
	cp $(BIN) tests/dmatest build/$(LHA_DIR)/
	awk -v version="$(FULL_VERSION)" '{ sub(/\[VERSION\]/, version); print }' \
		cachecop.readme > build/$(LHA_DIR)/CacheCop.readme
	cp README.md build/$(LHA_DIR)/README.md
	rm -f $@
	cd build && lha aqo5 $(LHA_OPTS) ../$@ $(LHA_DIR)
	rm -rf build/$(LHA_DIR)
	@echo "Created $@"

# Release floppy: bootable; booting it runs the self-test, and the tools
# can be copied off it when it's inserted into a running system.
DISK       = cachecop-$(FULL_VERSION).adf
DISK_TITLE = $(shell printf '%s' "CacheCop-$(FULL_VERSION)" | sed 's/-dirty$$//' | cut -c1-30)

disk: $(DISK)
$(DISK): $(BIN) tests/dmatest tests/Startup-Sequence cachecop.readme
	awk -v version="$(FULL_VERSION)" '{ sub(/\[VERSION\]/, version); print }' \
		cachecop.readme > build/CacheCop.readme
	rm -f $@
	xdftool $@ create + format "$(DISK_TITLE)" \
		+ write $(BIN) + write tests/dmatest \
		+ write build/CacheCop.readme CacheCop.readme \
		+ makedir S + write tests/Startup-Sequence S/Startup-Sequence \
		+ boot install
	@echo "Created $@"

# Same floppy under a fixed name, for local emulator runs
test-adf: $(DISK)
	cp $(DISK) cachecop-test.adf

clean:
	rm -rf build $(BIN) tests/dmatest cachecop-*.adf cachecop-*.lha

.PHONY: all lha disk test-adf clean FORCE
