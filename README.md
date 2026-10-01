# moppe-emu

Emulator for Mobira/Nokia "moppe" radios (1980s-90s NMT phones and PMR
radios converted to ham use), for developing and testing their firmware
without burning EPROMs.

Supported today: the **R58 series** (RB58/RC58/RD58) on P8E and P8N
processor cards with CU53AN or CU58AF handsets, the **Talkman
MD50/MD59/ME59** (CDP1802/1806) with CU53 or CU59 handsets, the
**MC25 TVL/PTL** (CDP1802) with its CU41 control unit, and the **Nokia
TMF-1 / TMN-1** (Talkman 520 / 620, uPD7810) with an HSN-2 or HSF-2
handset running its own firmware.

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
| Talkman board | `md5x.c`: MD50/MD59/ME59 memory maps, output latches, 4021 inputs, MAS7205 modem (100 Hz timer interrupt), synth capture, watchdog, ME59 ADC/DAC/8253; `md5x_api.c` (`libmd5x.so`), `python/md5xemu.py` harness, `python/md5xtui.py` terminal UI. Boots OH3NWQ mx5x v3.183 and OH1E #42 on all three models: [notes/md5x.md](notes/md5x.md) |
| MC25 board | `mc25.c`, `cu41.c`: KL1 ports, 1 ms interrupt, CU41 display and keypad, CCIR at digit level, soft-UART; `mc25_api.c` (`libmc25.so`), `python/mc25emu.py`, `python/mc25tui.py`. Runs OH5NXO/OH3NWQ mc25.asm v3.6: [notes/mc25.md](notes/mc25.md) |
| uPD7810 core | `upd7810.c`: uPD7810/78C10 instruction set with data-sheet state counts, timers, event counter, async serial, A/D, edge flags, interrupts. Unit test; decoder checked against as7810 on every instruction form |
| TMx-1 | `tmx1.c` radio unit (PLLs, DAC, LFU, 8253, modem at byte level, watchdog, power), `tmx1hs.c` HSN-2 / HSF-2 handsets (uPD7228 LCDs, keypad, LEDs, DTMF), bit-level MBUS between the two CPUs; `tmx1_api.c` (`libtmx1.so`), `python/tmx1emu.py`, `python/tmx1tui.py`, `python/upd7810dis.py`. Runs OH5NXO/OH3NWQ tmx1.asm v5.0 with HSN-2 v1.6 / HSF-2 v0.2: [notes/tmx1.md](notes/tmx1.md) |

Overview and conventions: [notes/emulator.md](notes/emulator.md); per radio (layers, timing model, fidelity evidence, limits): [r58](notes/r58.md), [md5x](notes/md5x.md), [mc25](notes/mc25.md), [tmx1](notes/tmx1.md).

## Build and test

```sh
make            # r58emu, libr58.so, libmd5x.so, libmc25.so, libtmx1.so
make test       # 8254, CDP1802 and uPD7810 unit tests
make test-md5x  # Talkman scenarios (needs the firmware repo's reference/md5x)
make test-mc25  # MC25 scenarios (needs the firmware repo's reference/mc25ptl)
make test-tmx1  # TMF-1/TMN-1 scenarios (needs reference/md5x/oh3nwq-moppe/tmx1_v50.zip)
make zex        # Z80 exerciser (zexdoc), ~75 s
```

The R58 scenario tests need a firmware image and run from the firmware
repo. The Talkman and MC25 tests build their firmware from the third-party
sources in the firmware repo's gitignored `reference/md5x/` and
`reference/mc25ptl/` (`MD5X_REF` / `MC25_REF` to point elsewhere) with the 2008 i386 `as06` binary, so they need a 32-bit
runtime; they skip otherwise. The TMx-1 tests do the same with OH3NWQ's
`tmx1_v50.zip` and its i386 `as7810` (`TMX1_REF`; as7810 also runs the
system `cpp`).

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

Or interactively (builds the firmware from the reference sources):

```sh
python3 python/md5xtui.py --fw mx5x-md59 --ram my.ram
python3 python/md5xtui.py --fw oh1e-me59 --script '196500#.'   # headless
python3 python/mc25tui.py --nv my-mc25.nv                       # MC25
python3 python/tmx1tui.py --fw tmf1 --hs hsn2 --ram my-tmf1.ram  # TMF-1
```

A TMF-1 with its HSN-2 (frequency as 6 digits in kHz, then #):

```python
from tmx1emu import Radio
r = Radio("tmf1.bin", "tmf1.lst", hs_rom="hsn2.bin", handset="HSN2")
r.run(6.0); r.type("433550#"); r.run(0.3)
print(r.display(), r.vco_hz())     # ('    30 2', '  433550', ' 0') 454950000.0
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
| `mc25.c`, `cu41.c`, `mc25_api.c` | MC25 TVL/PTL board, CU41 control unit, flat API |
| `upd7810.c` | uPD7810 / uPD78C10 core |
| `tmx1.c`, `tmx1hs.c`, `tmx1_api.c` | TMF-1/TMN-1 radio unit and MBUS, HSN-2/HSF-2 handsets, flat API |
| `api.c`, `main.c` | Flat API, CLI smoke run (`r58emu`) |
| `python/` | Harnesses (`r58emu.py`, `md5xemu.py`, `mc25emu.py`, `tmx1emu.py`), TUIs (`r58tui.py`, `md5xtui.py`, `mc25tui.py`, `tmx1tui.py`), AFSK decoder, uPD7810 disassembler |
| `tests/unit/`, `tests/zex/` | 8254, CDP1802 and uPD7810 unit tests, CP/M harness for zexdoc/zexall |
| `tests/md5x/`, `tests/mc25/`, `tests/tmx1/` | Talkman, MC25 and TMx-1 firmware builders (`roms.py`) and scenarios |
| `notes/` | Design notes |

Licence: MIT (`LICENSE`). `tests/zex/zex*.com` are Frank Cringle's Z80
instruction exercisers (third-party binaries, under their own terms).
