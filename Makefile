VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
CC      ?= cc
CFLAGS  ?= -O2 -g
override CFLAGS += -std=c23 -D_DEFAULT_SOURCE -D_GNU_SOURCE -D_DARWIN_C_SOURCE \
           -Wall -Wextra -Werror -Wconversion -Wshadow -Wvla -Wstrict-prototypes -Wimplicit-fallthrough -Wno-unused-parameter \
           -pthread -Isrc -DDBOX_VERSION='"$(VERSION)"'
PKG_CONFIG ?= pkg-config
DEPS     = yaml-0.1 libcurl sqlite3
override CFLAGS += $(shell $(PKG_CONFIG) --cflags $(DEPS) 2>/dev/null)
LDLIBS  += $(shell $(PKG_CONFIG) --libs $(DEPS) 2>/dev/null || echo -lyaml -lcurl -lsqlite3) -pthread

ifeq ($(shell uname -s),Darwin)
BREW := $(shell brew --prefix 2>/dev/null)
ifneq ($(BREW),)
override CFLAGS += -I$(BREW)/include
LDFLAGS += -L$(BREW)/lib
endif
LDLIBS  += -framework CoreServices
endif

LIB_SRC = $(filter-out src/main.c,$(wildcard src/*.c)) $(wildcard src/platform/*.c)
LIB_OBJ = $(LIB_SRC:src/%.c=build/%.o)
HEADERS = $(wildcard src/*.h) $(wildcard src/platform/*.h)

.PHONY: all test integration sanitize minio run clean

all: dbox

dbox: $(LIB_OBJ) build/main.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/test_dbox: $(LIB_OBJ) build/tests/test_dbox.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/test_s3: $(LIB_OBJ) build/tests/test_s3.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

build/tests/%.o: tests/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isrc -c -o $@ $<

test: build/test_dbox
	./build/test_dbox

# Needs `make minio` first.
integration: build/test_s3
	./build/test_s3

# Rebuilds everything with AddressSanitizer and UndefinedBehaviorSanitizer and runs the tests.
sanitize:
	$(MAKE) clean
	$(MAKE) test CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"
	$(MAKE) clean

minio:
	docker compose up -d --wait
	for s in minio-a minio-b; do \
		docker compose exec $$s sh -c 'mc alias set local http://localhost:9000 minioadmin minioadmin >/dev/null && mc mb -p local/dbox'; \
	done

run: dbox
	mkdir -p tmp/box
	./dbox run --config dev.config.yml

clean:
	rm -rf build dbox tmp
