CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -fPIC
OBJS     = z80.o daisy.o pio.o sio.o pit.o cu53an.o cu58af.o r58.o
MD5X_OBJS = cdp1802.o pit.o cu53an.o md5x.o
MC25_OBJS = cdp1802.o cu41.o mc25.o
TMX1_OBJS = upd7810.o pit.o tmx1hs.o tmx1.o
R40_OBJS = h8500.o h8532.o r40.o
MDR150_OBJS = cpu16.o hc16z1.o mdr150.o

all: r58emu libr58.so libmd5x.so libmc25.so libtmx1.so libr40.so libmdr150.so

r58emu: main.o $(OBJS)
	$(CC) $(CFLAGS) -o $@ main.o $(OBJS)

libr58.so: $(OBJS) api.o
	$(CC) -shared -o $@ $(OBJS) api.o

libmd5x.so: $(MD5X_OBJS) md5x_api.o
	$(CC) -shared -o $@ $(MD5X_OBJS) md5x_api.o

libmc25.so: $(MC25_OBJS) mc25_api.o
	$(CC) -shared -o $@ $(MC25_OBJS) mc25_api.o

libtmx1.so: $(TMX1_OBJS) tmx1_api.o
	$(CC) -shared -o $@ $(TMX1_OBJS) tmx1_api.o

libr40.so: $(R40_OBJS) r40_api.o
	$(CC) -shared -o $@ $(R40_OBJS) r40_api.o

libmdr150.so: $(MDR150_OBJS) mdr150_api.o
	$(CC) -shared -o $@ $(MDR150_OBJS) mdr150_api.o -lm

cpu16tab.h: tools/cpu16tab.py tools/cpu16_ops.json
	python3 tools/cpu16tab.py > $@

%.o: %.c *.h
	$(CC) $(CFLAGS) -c $<

test: r58emu tests/unit/test_pit tests/unit/test_cdp1802 tests/unit/test_upd7810 tests/unit/test_h8500 tests/unit/test_cpu16
	./tests/unit/test_pit
	./tests/unit/test_cdp1802
	./tests/unit/test_upd7810
	./tests/unit/test_h8500
	./tests/unit/test_cpu16

tests/unit/test_pit: tests/unit/test_pit.c pit.c pit.h
	$(CC) $(CFLAGS) -o $@ tests/unit/test_pit.c pit.c

tests/unit/test_cdp1802: tests/unit/test_cdp1802.c cdp1802.c cdp1802.h
	$(CC) $(CFLAGS) -o $@ tests/unit/test_cdp1802.c cdp1802.c

tests/unit/test_upd7810: tests/unit/test_upd7810.c upd7810.c upd7810.h
	$(CC) $(CFLAGS) -o $@ tests/unit/test_upd7810.c upd7810.c

tests/unit/test_h8500: tests/unit/test_h8500.c h8500.c h8500.h
	$(CC) $(CFLAGS) -o $@ tests/unit/test_h8500.c h8500.c

tests/unit/test_cpu16: tests/unit/test_cpu16.c tests/unit/test_cpu16_progs.h cpu16.c cpu16.h cpu16tab.h
	$(CC) $(CFLAGS) -o $@ tests/unit/test_cpu16.c cpu16.c

test-r40: libr40.so
	python3 -m unittest discover -s tests/r40

test-l8m: libr58.so
	python3 -m unittest discover -s tests/l8m

test-mdr150: libmdr150.so
	python3 -m unittest discover -s tests/mdr150

test-md5x: libmd5x.so
	python3 -m unittest discover -s tests/md5x

test-mc25: libmc25.so
	python3 -m unittest discover -s tests/mc25

test-tmx1: libtmx1.so
	python3 -m unittest discover -s tests/tmx1

refs:
	python3 tests/fetch_refs.py

ZEX = tests/zex/ZEXALL

zex: tests/zex/cpm
	@test -f $(ZEX)/zexdoc.com || { echo "$(ZEX) missing: git submodule update --init"; exit 1; }
	./tests/zex/cpm $(ZEX)/zexdoc.com

tests/zex/cpm: tests/zex/cpm.c z80.c z80.h
	$(CC) -O2 -o $@ tests/zex/cpm.c z80.c

clean:
	rm -f *.o r58emu libr58.so libmd5x.so libmc25.so libtmx1.so libr40.so libmdr150.so tests/unit/test_pit tests/unit/test_cdp1802 tests/unit/test_upd7810 tests/unit/test_h8500 tests/unit/test_cpu16 tests/zex/cpm

.PHONY: all test test-md5x test-mc25 test-tmx1 test-r40 test-mdr150 test-l8m refs zex clean
