# dcjson - fast terminal JSON viewer
CC       ?= gcc
PREFIX   ?= /usr/local
CPPFLAGS += -D_GNU_SOURCE -Isrc
STD      := -std=c17
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wno-format-nonliteral

SRC      := $(wildcard src/*.c)
HDR      := $(wildcard src/*.h)
LIB_SRC  := $(filter-out src/main.c,$(SRC))

# Release build: -O2 (the parser's inner loops are worth it), LTO,
# dead-section GC, no unwind tables, stripped.  Stack protector and
# FORTIFY stay on.
OPT             ?= -O2
RELEASE_CFLAGS  := $(STD) $(OPT) -flto=auto -s $(WARN) -D_FORTIFY_SOURCE=2 \
                   -ffunction-sections -fdata-sections \
                   -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident
RELEASE_LDFLAGS := -flto=auto -s -Wl,--gc-sections -Wl,-O1 -Wl,--as-needed -Wl,--hash-style=gnu \
                   -Wl,--build-id=none

DEBUG_CFLAGS    := $(STD) -O0 -g3 $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS   := -fsanitize=address,undefined

.PHONY: all release debug test bench clean install uninstall

all: release

release: dcjson

dcjson: $(SRC) $(HDR)
	$(CC) $(CPPFLAGS) $(RELEASE_CFLAGS) $(SRC) -o $@ $(RELEASE_LDFLAGS)

debug: dcjson-debug

dcjson-debug: $(SRC) $(HDR)
	$(CC) $(CPPFLAGS) $(DEBUG_CFLAGS) $(SRC) -o $@ $(DEBUG_LDFLAGS)

build/tests: $(LIB_SRC) $(HDR) tests/test.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(DEBUG_CFLAGS) $(LIB_SRC) tests/test.c -o $@ $(DEBUG_LDFLAGS)

build/gen: tests/gen.c
	@mkdir -p build
	$(CC) $(STD) -O2 $(WARN) tests/gen.c -o $@

test: build/tests dcjson-debug
	ASAN_OPTIONS=detect_leaks=1 ./build/tests ./dcjson-debug

bench: dcjson build/gen
	sh tests/bench.sh ./dcjson

install: dcjson
	install -D -m 0755 dcjson $(DESTDIR)$(PREFIX)/bin/dcjson

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/dcjson

clean:
	rm -rf dcjson dcjson-debug build
