CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -fPIC
OBJS     = z80.o daisy.o pio.o sio.o pit.o cu53an.o cu58af.o r58.o
MD5X_OBJS = cdp1802.o pit.o cu53an.o md5x.o

all: r58emu libr58.so libmd5x.so

r58emu: main.o $(OBJS)
	$(CC) $(CFLAGS) -o $@ main.o $(OBJS)

libr58.so: $(OBJS) api.o
	$(CC) -shared -o $@ $(OBJS) api.o

libmd5x.so: $(MD5X_OBJS) md5x_api.o
	$(CC) -shared -o $@ $(MD5X_OBJS) md5x_api.o

%.o: %.c *.h
	$(CC) $(CFLAGS) -c $<

test: r58emu tests/unit/test_pit tests/unit/test_cdp1802
	./tests/unit/test_pit
	./tests/unit/test_cdp1802

tests/unit/test_pit: tests/unit/test_pit.c pit.c pit.h
	$(CC) $(CFLAGS) -o $@ tests/unit/test_pit.c pit.c

tests/unit/test_cdp1802: tests/unit/test_cdp1802.c cdp1802.c cdp1802.h
	$(CC) $(CFLAGS) -o $@ tests/unit/test_cdp1802.c cdp1802.c

test-md5x: libmd5x.so
	python3 -m unittest discover -s tests/md5x

zex: tests/zex/cpm
	./tests/zex/cpm tests/zex/zexdoc.com

tests/zex/cpm: tests/zex/cpm.c z80.c z80.h
	$(CC) -O2 -o $@ tests/zex/cpm.c z80.c

clean:
	rm -f *.o r58emu libr58.so libmd5x.so tests/unit/test_pit tests/unit/test_cdp1802 tests/zex/cpm

.PHONY: all test test-md5x zex clean
