# tuple-2048 – build, test, train, site
CC      ?= cc
GIT_REV := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
OPT     ?= -O3 -march=native -flto
CFLAGS  := $(OPT) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Wshadow -DGIT_REV=\"$(GIT_REV)\"
LDLIBS  += -lm -lpthread

SRC     := src/board.c src/ntuple.c src/td.c src/search.c src/io.c src/train.c
OBJ     := $(SRC:src/%.c=build/%.o)
HDR     := $(wildcard src/*.h)

THREADS ?= $(shell nproc 2>/dev/null || echo 4)
GAMES   ?= 100000

.PHONY: all test bench clean train-small train site codepage-check

all: t2048

build/%.o: src/%.c $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

t2048: $(OBJ) build/main.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

build/test_main: tests/test_main.c $(OBJ) $(HDR)
	$(CC) $(CFLAGS) tests/test_main.c $(OBJ) -o $@ $(LDLIBS)

test: build/test_main
	./build/test_main
	python3 tools/gen_codepage.py --check
	python3 -m unittest discover -s tests -p 'test_*.py' -q

bench: t2048
	./t2048 bench

# Quick sanity run: small network, a few seconds.
train-small: t2048
	./t2048 train --net small --games $(GAMES) --threads $(THREADS) --every 1000 --out runs/small

# The real thing: 4x6-tuple network, optimistic init, TC after 1/2 of the run.
train: t2048
	./t2048 train --net strong --games $(GAMES) --threads $(THREADS) --init 160000 \
	    --tc-after $$(( $(GAMES) / 2 )) --stages 14 --every 10000 --out runs/strong

# Regenerate web/data/*.js from runs/ and the annotated sources.
site:
	python3 tools/gen_codepage.py
	python3 tools/bundle.py

codepage-check:
	python3 tools/gen_codepage.py --check

clean:
	rm -rf build t2048
