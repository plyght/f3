CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
PREFIX ?= /usr/local
FFF_DIR ?= vendor/fff
FFF_TARGET_DIR ?= $(FFF_DIR)/target/release

UNAME_S := $(shell uname -s)
FFF_LIBNAME := fff_c

ifeq ($(UNAME_S),Darwin)
FFF_LIB_EXT := dylib
FFF_RPATH := -Wl,-rpath,@executable_path
else
FFF_LIB_EXT := so
FFF_RPATH := -Wl,-rpath,$$ORIGIN
endif

FFF_LIB_FILE := lib$(FFF_LIBNAME).$(FFF_LIB_EXT)
FFF_LIB := $(FFF_TARGET_DIR)/$(FFF_LIB_FILE)

.PHONY: all fff clean install run

all: f3 f3g

fff:
	cargo build --release --manifest-path $(FFF_DIR)/Cargo.toml --package fff-c
	cp $(FFF_DIR)/crates/fff-c/include/fff.h include/fff.h

f3: src/main.c include/fff.h $(FFF_LIB)
	$(CC) $(CFLAGS) -Iinclude src/main.c -L$(FFF_TARGET_DIR) -l$(FFF_LIBNAME) $(FFF_RPATH) -lz -ldl -lpthread -lm -o f3
	cp $(FFF_LIB) ./$(FFF_LIB_FILE)

f3g: f3
	ln -sf f3 f3g

$(FFF_LIB): fff

install: f3 f3g
	install -d $(PREFIX)/bin
	install -m 755 f3 $(PREFIX)/bin/f3
	ln -sf f3 $(PREFIX)/bin/f3g

run: f3
	./f3

clean:
	rm -f f3 f3g
