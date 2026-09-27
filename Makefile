CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -fPIC
OBJS     = z80.o daisy.o pio.o sio.o pit.o cu53an.o cu58af.o r58.o

all: r58emu libr58.so

r58emu: main.o $(OBJS)
	$(CC) $(CFLAGS) -o $@ main.o $(OBJS)

libr58.so: $(OBJS) api.o
	$(CC) -shared -o $@ $(OBJS) api.o

%.o: %.c *.h
	$(CC) $(CFLAGS) -c $<

test: r58emu tests/unit/test_pit
	./tests/unit/test_pit

tests/unit/test_pit: tests/unit/test_pit.c pit.c pit.h
	$(CC) $(CFLAGS) -o $@ tests/unit/test_pit.c pit.c

zex: tests/zex/cpm
	./tests/zex/cpm tests/zex/zexdoc.com

tests/zex/cpm: tests/zex/cpm.c z80.c z80.h
	$(CC) -O2 -o $@ tests/zex/cpm.c z80.c

clean:
	rm -f *.o r58emu libr58.so tests/unit/test_pit tests/zex/cpm

.PHONY: all test zex clean
