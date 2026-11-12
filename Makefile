CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c11
LDLIBS  := $(shell pkg-config --libs sqlite3 2>/dev/null || echo -lsqlite3)
CFLAGS  += $(shell pkg-config --cflags sqlite3 2>/dev/null)
PREFIX  ?= /usr/local

SRC := src/main.c src/parse.c src/extract.c src/db.c src/util.c
OBJ := $(SRC:.c=.o)

frt: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/frt.h src/db.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: frt
	install -Dm755 frt $(DESTDIR)$(PREFIX)/bin/frt

test: frt
	tests/run.sh ./frt

test-bot: frt
	$(if $(wildcard bot/.venv/bin/python),bot/.venv/bin/python,python3) tests/test_bot.py ./frt

clean:
	rm -f frt $(OBJ)
	rm -rf tests/out

.PHONY: install clean test test-bot
