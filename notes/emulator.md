# Emulator

Several boards share CPU cores and chip models; each radio has its own
note with its layers, model table, fidelity evidence and known limits.

| Radio | CPU | Board, handset | Note |
|---|---|---|---|
| R58 series (P8E / P8N) | `z80.c` | `r58.c`; `cu53an.c`, `cu58af.c` | [r58.md](r58.md) |
| Talkman MD50 / MD59 / ME59 | `cdp1802.c` (1802 / 1806) | `md5x.c`; `cu53an.c` | [md5x.md](md5x.md) |
| MC25 TVL / PTL | `cdp1802.c` | `mc25.c`; `cu41.c` | [mc25.md](mc25.md) |
| TMF-1 / TMN-1 (Talkman 520 / 620) | `upd7810.c` (radio and handset) | `tmx1.c`; `tmx1hs.c` (HSN-2, HSF-2) | [tmx1.md](tmx1.md) |
| R40 (RC40 / RD40) | `h8500.c`, `h8532.c` (H8/532) | `r40.c` (L100, CU43) | [r40.md](r40.md) |
| Comarco MDR150 | `cpu16.c`, `hc16z1.c` (68HC16Z1) | `mdr150.c` | [mdr150.md](mdr150.md) |

Shared chips: `pit.c` (Intel 8254 / 8253: R58, ME59, TMx-1), `pio.c`,
`sio.c`, `daisy.c` (R58).

## Common conventions

- Instruction-stepped cores with exact cycle counts from the data sheets;
  peripherals advance after each instruction, I/O happens at instruction
  granularity. Each board keeps time in its own clock unit (see its note).
- Each board has a flat C API (`*api.c` → `lib*.so`) and a Python ctypes
  harness `python/<board>emu.py` with a `Radio` class: `run()`, `step()`,
  `press()` / `type()`, `display()`, `events` / `take_events()`,
  breakpoints, watchpoints, a PC trace and symbol lookup from the
  firmware's listing or map. A curses TUI `python/<board>tui.py` with a
  headless `--script` mode sits on top.
- Third-party test firmware is built by each board's `tests/<board>/roms.py`
  from a gitignored `reference/`: this repo's (`make refs` fetches it from
  the authors' sites, README "Test firmware") or the firmware repo's. It
  is never committed.

## Changing the emulator

Any change: `make test`, the board scenario suites (`make test-md5x`,
`test-mc25`, `test-tmx1`, `test-r40`, `test-mdr150`), and from the firmware repo its test suite
(`python3 tools/ci/runtests.py`) and `tools/r58/emuoracle.py` against the old
library (run it with `R58_LIB=` the old library and diff): behaviour must
stay bit-identical unless the change is meant to alter it.

## Several radios

Radios are independent objects, so a test can run several side by side
(the R58 RF link is in [r58.md](r58.md)). A TMx-1 object holds two CPUs
itself, the radio and its handset, linked by MBUS.

## Debugging workflow

The harnesses share the same calls; the R58 example is in
[r58.md](r58.md). A run resumed at a breakpoint executes that one
instruction before breakpoints apply again (fixed 2026-09-28 in the R58
board: the skip used to stay armed until the next breakpoint hit
anywhere, so a stop right after `call()` or at a different address could
be missed; the later boards copy the fixed logic).
