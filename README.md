# moppe-emu

Emulator for Mobira/Nokia "moppe" radios (1980s-90s NMT phones and PMR
radios converted to ham use), for developing and testing their firmware
without burning EPROMs.

Supported today: the **R58 series** (RB58/RC58/RD58) on P8E and P8N
processor cards with CU53AN or CU58AF handsets and the RB58VY's L8M
logic board, the **Talkman
MD50/MD59/ME59** (CDP1802/1806) with CU53 or CU59 handsets, the
**MC25 TVL/PTL** (CDP1802) with its CU41 control unit, and the **Nokia
TMF-1 / TMN-1** (Talkman 520 / 620, uPD7810) with an HSN-2 or HSF-2
handset running its own firmware, and the **Nokia R40** (RC40/RD40,
Hitachi H8/532) running its original Nokia firmware with a CU43 control
head, and the **Comarco MDR150** data radio (68HC16Z1) running OH5NXO's
HaMDR ham firmware.

The firmware it was built for, and its scenario and differential tests,
live in the firmware repo (`moppe`), which has this repo as the `emu/`
submodule.

## Status (2026-10-05)

| Area | State |
|---|---|
| Z80 core | `z80.c`: instruction-stepped, exact T-states, undocumented opcodes and flags. Passes zexdoc and zexall (`make zex`). |
| Chips | Z80 PIO, Z80 SIO (async), Intel 8254, IM2 daisy chain |
| Handsets | CU53AN (PCF2111 LCDs, shift chain, 74C923 keypad), CU58AF (I²C: PCF8574, PCF8576, PCD3312) |
| R58 board | `r58.c`: P8E/P8N memory maps incl. the banked ROM window, and the RB58VY L8M board (EEPROM, one-board I/O map, S8M synth; runs OH5NXO's R58bis for L8M; the original Nokia ROM lacks EPROM1: [notes/r58.md](notes/r58.md)), I/O, ADC/DACs, latches, watchdog, power, FX429 modem at byte level, synthesizer capture, tone pin edges, breakpoints and watchpoints |
| API | `api.c` flat C API (`libr58.so`); `python/r58emu.py` ctypes harness (`Radio`); `python/r58tui.py` terminal UI; `python/afsk.py` AX.25 decoder |
| CDP1802 core | `cdp1802.c`: CDP1802 and CDP1806 (68xx instructions, counter/timer), exact machine cycles. Unit test, no exerciser |
| Talkman board | `md5x.c`: MD50/MD59/ME59 memory maps, output latches, 4021 inputs, MAS7205 modem (100 Hz timer interrupt), synth capture, watchdog, ME59 ADC/DAC/8253; `md5x_api.c` (`libmd5x.so`), `python/md5xemu.py` harness, `python/md5xtui.py` terminal UI. Boots OH3NWQ mx5x v3.183 and OH1E #42 on all three models: [notes/md5x.md](notes/md5x.md) |
| MC25 board | `mc25.c`, `cu41.c`: KL1 ports, 1 ms interrupt, CU41 display and keypad, CCIR at digit level, soft-UART; `mc25_api.c` (`libmc25.so`), `python/mc25emu.py`, `python/mc25tui.py`. Runs OH5NXO/OH3NWQ mc25.asm v3.6: [notes/mc25.md](notes/mc25.md) |
| uPD7810 core | `upd7810.c`: uPD7810/78C10 instruction set with data-sheet state counts, timers, event counter, async serial, A/D, edge flags, interrupts. Unit test; decoder checked against as7810 on every instruction form |
| H8/500 core | `h8500.c` (maximum mode) and `h8532.c` (H8/532 ports, FRT1-3, 8-bit timer, SCI, A/D, WDT, interrupt controller). Unit test |
| R40 | `r40.c`: L100 logic board (latches, serial bus, PLLs, PCF8584, FX429 at byte level) and CU43 control head (PCF8574 keypad, PCF8578/79 LCD, 24C02 service key); `r40_api.c` (`libr40.so`), `python/r40emu.py`. Boots the RC40 firmware Cr 13.04 to its self test and error display, and into the LOCAL service mode with a service head: [notes/r40.md](notes/r40.md) |
| CPU16 core | `cpu16.c` with `cpu16tab.h` generated from the Reference Manual's instruction table (`tools/cpu16tab.py`): whole instruction set incl. the MAC unit, manual clock counts plus bus-access cost. Unit test with hc16-assembled programs; `python/cpu16dis.py` |
| MDR150 | `hc16z1.c` (SIM chip selects, ports, PIT, watchdog, standby RAM, GPT with PWM, QSM SCI, ADC), `mdr150.c` (Am29F010, RAM, 4094 + MB1504 radio module, AFSK audio in/out, serial mux); `mdr150_api.c` (`libmdr150.so`), `python/mdr150emu.py`, `python/mdr150tui.py` (serial console, radio state, AX.25 log, APRS packets on the air on demand). Runs OH5NXO's HaMDR 174: console, config to flash, APRS beacon out, AFSK in, digipeating, KISS: [notes/mdr150.md](notes/mdr150.md) |
| TMx-1 | `tmx1.c` radio unit (PLLs, DAC, LFU, 8253, modem at byte level, watchdog, power), `tmx1hs.c` HSN-2 / HSF-2 handsets (uPD7228 LCDs, keypad, LEDs, DTMF), bit-level MBUS between the two CPUs; `tmx1_api.c` (`libtmx1.so`), `python/tmx1emu.py`, `python/tmx1tui.py`, `python/upd7810dis.py`. Runs OH5NXO/OH3NWQ tmx1.asm v5.0 with HSN-2 v1.6 / HSF-2 v0.2: [notes/tmx1.md](notes/tmx1.md) |

Overview and conventions: [notes/emulator.md](notes/emulator.md); per radio (layers, timing model, fidelity evidence, limits): [r58](notes/r58.md), [md5x](notes/md5x.md), [mc25](notes/mc25.md), [tmx1](notes/tmx1.md), [r40](notes/r40.md), [mdr150](notes/mdr150.md).

## Build and test

```sh
make            # r58emu, libr58.so, libmd5x.so, libmc25.so, libtmx1.so, libr40.so, libmdr150.so
make test       # 8254, CDP1802, uPD7810, H8/500 and CPU16 unit tests
make refs       # fetch the test firmware sources (see "Test firmware")
make test-md5x  # Talkman scenarios
make test-mc25  # MC25 scenarios
make test-tmx1  # TMF-1/TMN-1 scenarios
make test-r40   # R40 scenarios (original Nokia firmware)
make test-mdr150 # MDR150 scenarios (HaMDR 174)
make test-l8m   # RB58VY L8M board (OH5NXO's R58bis, Nokia EPROM0)
make zex        # Z80 exerciser (zexdoc, from the ZEXALL submodule), ~75 s
```

The Talkman, MC25 and TMx-1 tests build their firmware with the
assemblers the firmware authors shipped: `as06` (2008) and `as7810`
(2004), i386 Linux binaries, so they need a 32-bit runtime
(`/lib/ld-linux.so.2`); as7810 also runs the system `cpp`. Without them,
or without the sources, the tests skip. The R58 scenario tests are in
the firmware repo.

## Test firmware

None of the firmware the emulator runs is in this repository: it is
other people's work under their own terms. `make refs`
(`tests/fetch_refs.py`) downloads what the tests need from the authors'
own sites into `reference/` (gitignored), checking each file against the
hash of what was published on 2026-10-02:

| Files | From | Terms |
|---|---|---|
| `mx5x.asm`, `md59_v318.zip` (as06): Talkman MD50/MD59/ME59 v3.183 by OH3NWQ | github.com/oh3nwq/moppe | CC BY-NC-SA 3.0 (in the source) |
| `md50bis/md50.asm`: Talkman rewrite #42 by OH1E | titanix.net/DMR/md50/ | no licence given |
| `mc25.asm`, `as06`: MC25 TVL/PTL v3.6 by OH5NXO / OH3NWQ | oh3tr.fi/~ftp/modifications/mobira/mc25ptl/ | no licence given |
| `tmx1_v50.zip`: TMF-1/TMN-1 v5.0, HSN-2/HSF-2 handsets and as7810, by OH5NXO / OH3NWQ / OK2UCX | github.com/oh3nwq/moppe | OH3NWQ's licence (in the zip and the source): licensed amateur use; read it before use |
| `r58p8x3Z.bin.als`, `r58.asm.als`: R58 v3_Z ALs by OH1E / OH5NXO | titanix.net/DMR/r58/ | no licence given |
| `rc40_rom/ABSBIN`: original Nokia RC40 firmware Cr 13.04-0 (1993), streamed out of OH5NXO's 339 MB archive `oh5nxo.mods.2018.tar.gz` | oh3tr.fi/~ftp/modifications/sorsat/ | Nokia's; for testing only, never redistribute |
| `MDR150/hamdr/hamdr.hex`, `bootstrap`: HaMDR 174 (2012) and the MDR150 bootstrap (2009) by OH5NXO, from the same archive, in the same pass | oh3tr.fi/~ftp/modifications/sorsat/ | no licence given |
| `R58vy/rom.0`: original Nokia RB58VY EPROM0; `R58bis/R58/L8M.bin`: OH5NXO's R58bis for L8M (2014), from the same archive, in the same pass | oh3tr.fi/~ftp/modifications/sorsat/ | Nokia's (testing only, never redistribute); OH5NXO's: no licence given |

The assemblers are OH5NXO's "jas", with no licence statement. Fetching a
file here is the same as downloading it from its author; keep it out of
anything you publish. A firmware repo checkout next to the emulator
(`emu/` as its submodule) supplies the same files from its own
`reference/` instead, and `MD5X_REF` / `MC25_REF` / `TMX1_REF` / `MDR150_REF` / `L8M_REF` point the
builders anywhere else.

The R58 runs either the firmware repo's build (with its linker map for
symbols) or, alone, the published ALs binary without symbols:
`python3 python/r58tui.py` picks that after `make refs`.

Running a ROM:

```sh
python3 python/r58tui.py --rom r58.bin --lst r58.map --nv my.nv
python3 python/r58tui.py --rom reference/r58/r58p8x3Z.bin.als --lst ''   # no symbols
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
| `h8500.c`, `h8532.c`, `r40.c`, `r40_api.c` | H8/500 core, H8/532 on-chip modules, R40 L100 board and CU43, flat API |
| `api.c`, `main.c` | Flat API, CLI smoke run (`r58emu`) |
| `python/` | Harnesses (`r58emu.py`, `md5xemu.py`, `mc25emu.py`, `tmx1emu.py`, `r40emu.py`), TUIs (`r58tui.py`, `md5xtui.py`, `mc25tui.py`, `tmx1tui.py`), AFSK decoder, uPD7810 disassembler |
| `tests/unit/`, `tests/zex/` | 8254, CDP1802 and uPD7810 unit tests, CP/M harness for zexdoc/zexall (`tests/zex/ZEXALL` submodule) |
| `tests/md5x/`, `tests/mc25/`, `tests/tmx1/`, `tests/r40/` | Talkman, MC25, TMx-1 firmware builders and the R40 ROM finder (`roms.py`), scenarios |
| `tests/fetch_refs.py` | `make refs`: fetches the test firmware from its authors' sites |
| `notes/` | Design notes |

Licence: MIT (`LICENSE`). Frank Cringle's Z80 instruction exercisers
(zexdoc, zexall; GPL v2) are not part of this repo: `tests/zex/ZEXALL` is
a submodule of [agn453/ZEXALL](https://github.com/agn453/ZEXALL) (clone
with `--recursive`, or `git submodule update --init`).
