# moppe-emu

Emulator for Mobira/Nokia "moppe" radios (1980s-90s NMT phones and PMR
radios converted to ham use), for developing and testing their firmware
without burning EPROMs.

Supported today: the **R58 series** (RB58/RC58/RD58) on P8E and P8N
processor cards with CU53AN or CU58AF handsets, and the **Talkman
MD50/MD59/ME59** (CDP1802/1806) with CU53 or CU59 handsets.

The firmware it was built for, and its scenario and differential tests,
live in the firmware repo (`moppe`), which has this repo as the `emu/`
submodule.

## Status (2026-10-02)

| Area | State |
|---|---|
| Z80 core | `z80.c`: instruction-stepped, exact T-states, undocumented opcodes and flags. Passes zexdoc and zexall (`make zex`). |
| Chips | Z80 PIO, Z80 SIO (async), Intel 8254, IM2 daisy chain |
| Handsets | CU53AN (PCF2111 LCDs, shift chain, 74C923 keypad), CU58AF (I²C: PCF8574, PCF8576, PCD3312) |
| R58 board | `r58.c`: P8E/P8N memory maps incl. the banked ROM window, I/O, ADC/DACs, latches, watchdog, power, FX429 modem at byte level, synthesizer capture, tone pin edges, breakpoints and watchpoints |
| API | `api.c` flat C API (`libr58.so`); `python/r58emu.py` ctypes harness (`Radio`); `python/r58tui.py` terminal UI; `python/afsk.py` AX.25 decoder |
| CDP1802 core | `cdp1802.c`: CDP1802 and CDP1806 (68xx instructions, counter/timer), exact machine cycles. Unit test, no exerciser |
| Talkman board | `md5x.c`: MD50/MD59/ME59 memory maps, output latches, 4021 inputs, MAS7205 modem (100 Hz timer interrupt), synth capture, watchdog, ME59 ADC/DAC/8253; `md5x_api.c` (`libmd5x.so`), `python/md5xemu.py`. Boots OH3NWQ mx5x v3.183 and OH1E #42 on all three models: [notes/md5x.md](notes/md5x.md) |

Design, timing model and fidelity evidence: [notes/emulator.md](notes/emulator.md).

## Build and test

```sh
make            # r58emu, libr58.so, libmd5x.so
make test       # 8254 and CDP1802 unit tests
make test-md5x  # Talkman scenarios (needs the firmware repo's reference/md5x)
make zex        # Z80 exerciser (zexdoc), ~75 s
```

The R58 scenario tests need a firmware image and run from the firmware
repo. The Talkman tests build their firmware from the third-party sources
in the firmware repo's gitignored `reference/md5x/` (`MD5X_REF` to point
elsewhere) with the 2008 i386 `as06` binary, so they need a 32-bit
runtime; they skip otherwise.

Running a ROM:

```sh
python3 python/r58tui.py --rom r58.bin --lst r58.map --nv my.nv
```

```python
import sys; sys.path.insert(0, "python")
from r58emu import Radio
r = Radio("r58.bin", "r58.map", nv=open("my.nv", "rb").read())
r.run(2.5); r.type("433500"); r.press("#"); r.run(0.3)
print(r.display(), r.rx_hz())
```

A Talkman (OH3NWQ firmware: 5 digits, the 100 MHz digit implied):

```python
from md5xemu import Radio
r = Radio("mx5x-md59.bin", "mx5x-md59.lst", model="MD59", cu="CU59")
r.run(2.5); r.type("33500"); r.run(0.5)
print(r.display(), r.rx_hz())      # ('30    ', '20  433500') 433500000.0
```

## Layout

| Path | What |
|---|---|
| `z80.c` | Z80 core |
| `cdp1802.c` | CDP1802/1806 core |
| `pio.c`, `sio.c`, `pit.c`, `daisy.c` | Zilog/Intel chips |
| `cu53an.c`, `cu58af.c` | Handsets |
| `r58.c` | R58 board |
| `md5x.c`, `md5x_api.c` | Talkman MD50/MD59/ME59 board, its flat API |
| `api.c`, `main.c` | Flat API, CLI smoke run (`r58emu`) |
| `python/` | Harnesses (`r58emu.py`, `md5xemu.py`), TUI, AFSK decoder |
| `tests/unit/`, `tests/zex/` | 8254 and CDP1802 unit tests, CP/M harness for zexdoc/zexall |
| `tests/md5x/` | Talkman firmware builder (`roms.py`) and scenarios |
| `notes/` | Design notes |

Licence: MIT (`LICENSE`). `tests/zex/zex*.com` are Frank Cringle's Z80
instruction exercisers (third-party binaries, under their own terms).
