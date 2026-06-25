CC      := cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -g -D_POSIX_C_SOURCE=200809L -Isrc
ASAN    := -fsanitize=address,undefined
SRC     := $(wildcard src/*.c)
V2CORE  := $(wildcard src/v2/*.c)
CORE    := src/block.c src/alloc.c src/super.c src/inode.c src/dir.c src/xattr.c src/journal.c src/crypto.c src/csum.c src/fs.c $(V2CORE)
LDLIBS  := -lcrypto -lpthread -lz
FUSEFLAGS := $(shell pkg-config --cflags --libs fuse3)

TESTS := $(patsubst tests/%.c,build/%,$(wildcard tests/test_*.c))

build/%: tests/%.c $(CORE)
	@mkdir -p build
	$(CC) $(CFLAGS) $< $(CORE) -o $@ $(LDLIBS)

.PHONY: test test-asan cli fuse clean
test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t =="; ./$$t || exit 1; done

test-asan: CFLAGS += $(ASAN)
test-asan: clean test

cli: $(CORE) src/cli.c
	@mkdir -p build
	$(CC) $(CFLAGS) src/cli.c $(CORE) -o build/ghostfs-cli $(LDLIBS)

fuse: $(CORE) src/fuse_main.c
	@mkdir -p build
	$(CC) $(CFLAGS) -D_FILE_OFFSET_BITS=64 src/fuse_main.c $(CORE) $(FUSEFLAGS) -o build/ghostfs $(LDLIBS) -lpthread

clean:
	rm -rf build
