# Talkman MD50 / MD59 / ME59

Status (2026-10-02): boots and runs both ham firmware lineages on all
three models: OH3NWQ `mx5x.asm` v3.183 (polled, timing loops) and OH1E
`md50.asm` #42 (100 Hz modem interrupt, IDL, CDP1806 counter code).
Hardware facts and sources are in the firmware repo's `notes/md5x.md`.

## Layers

| Layer | Files | Notes |
|---|---|---|
| CPU | `cdp1802.c` | CDP1802, and CDP1806 (MD59/ME59): all 68xx instructions, counter/timer (timer, event and pulse modes, ETQ, CIL/CIE/XIE), BCD. Instruction-stepped, machine cycles exact (2, long branches/skips 3, 68xx 3-10; interrupt entry 1, IDL 1 per cycle). Unit test `tests/unit/test_cdp1802.c` (hand-assembled programs). |
| Board | `md5x.c` | Memory maps, N0/N1/N2 I/O decode, eight 4-bit output latches (OUT 4, latch from R(X) bits 3..1), 4021 input chain (Q clock, PSC load, /EF3), MAS7205 modem at register level, serial synth capture, watchdog, OFF1 power-off, ME59 ADC/DAC/8253, MD50 WAIT (N0) |
| Handset | `cu53an.c` | Same chain/LCD protocol as the R58 CU53AN; parallel-load bit 1 (`bit1`) is the Talkman D5 key bit, idle 1 on CU53, 0 on CU59 |
| API | `md5x_api.c` → `libmd5x.so`; `python/md5xemu.py`; `python/md5xtui.py` (curses, or `--script` headless) | `Radio(rom, listing, model, cu)`; as06 listing symbols; LCD text decoded with the ROM's own `font` table |
| Tests | `tests/md5x/` (`make test-md5x`) | `roms.py` builds six firmware images from the reference sources (`make refs`, or the firmware repo's `reference/md5x/`) with as06; `test_md5x.py` (13 scenarios). Skip if the sources or a 32-bit runtime are missing |

Time is in CPU clock periods (3.6864 MHz MD5x, 4.8 MHz ME59); peripherals
advance after each instruction.

## Model

| Item | Model | Source |
|---|---|---|
| ROM | 0000-7FFF | PE2A text; ME59 manual |
| RAM | MD50 1 KB mirrored in 8000-BFFF; MD59/ME59 2 KB (or 8 KB) mirrored in 8000-9FFF | PE2A (8000-83FF), ME59 manual; mirroring is assumed from the partial decode |
| MD50 ID PROM | C000-FFFF, 4 bits, 0xFF unless loaded | PE2A/AP1; its A4-A7 come from latches (not modelled) |
| ME59 | A000 AFC DAC (event), E000-E007 ADC (write starts, read returns the last conversion), 8253 on N0 (CLK0 = TPB, CLK1 = 455 kHz, CLK2 = 19.2 kHz; gates high) | ME59 manual, OH1E `me59_init` |
| N lines | decoded one per line: N0 (1) MD50 WAIT / ME59 8253, N1 (2) modem, N2 (4) latches | PE2A text, firmware constants HLT/TMR=1, MDM=2, REG=4 |
| Modem | status RFLAG HM TFLAG TXE FFSK CFLAG GPIN1 GPIN2; control RINTE TINTE CINTE .. CCF CRTF; 100 Hz CFLAG timer; /INT = enabled flags; data byte level | OH5NXO `MAS.registers` (valid for 7205 and 7825), PE2A text |
| GPIN1 / GPIN2 | clipped RX audio (`set_rx_tone`) / MD5x squelch SQ (`set_squelch`), ME59 8253 OUT2 | PE2A, sources |
| Inputs | 4021: P8 = DA, then per-model order from the firmware's `input_register`; serial in last; /EF1 DCU, /EF3 Q8, /EF4 /PTT; MD59 /EF2 = RF_OFF | firmware; MD50 polarities match the PE2A text |
| TOFF | follows XM (no output while XM = 0); `MD5X_IN_TOFF` forces a TX fault | needed by OH1E's `txoff` |
| PTT | `ptt` input asserts /EF4 and the model's PTT bit (MD50 PC, MD59 AC, ME59 /PTT) | both firmwares' input reads |
| Synth | while SWE: each memory write clocks D7 (MD5x) / D0 (ME59), inverted. MD5x SE strobe: SW1 SW2 N9..N0 A6..A0, VCO = (80N+A)·25 kHz; ME59 RSE: SW1 SW2 b16..b0 (12.5 kHz units); ME59 TSE: offset PLL N, A (32/33) | mx5x `txsynth`, `dooffset`; PE2A ("SWE gates D7 and MWR") |
| Watchdog | counts while WDR = 0; /CLEAR after 106 ms (MD5x) / 213 ms (ME59); resets CPU, modem, latches | PE2A text (106/213 ms, maybe off by 2), ME59 manual, OH1E "level active, 1 resets the count" |
| Power | OFF1 rising edge: MD50 latch 4 bit 0, MD59 latch 3 bit 1, ME59 latch 7 bit 1 | sources |

## Fidelity evidence

- The cycle-counted 1750 Hz loop (`waitkeybeep`, 132 / 171 machine
  cycles per half period) measures 1745.6 Hz on MD59 and 1754.4 Hz on
  ME59 from the emulated PHI edges.
- OH1E's tick register R0 advances 100 per second (modem timer
  interrupt, RET/DIS, SAV).
- Frequency entry, PTT (X314 +10 MHz TX shift), squelch LEDs/audio gates,
  power switch ("BYE-BYE 73", OFF1), battery RAM across a power cycle,
  watchdog reset of a stuck loop: `tests/md5x/test_md5x.py`.
- The emulated LCD equals mx5x's `segments[]` RAM at the end of every
  `display()` call (5 s on each model, 130 calls).
- All six builds run 5 s without a watchdog reset or an undefined
  opcode; the modem timer gives 500 ticks in 5 s.

## Known limits

| Item | State |
|---|---|
| 68xx cycle counts | from the CDP1805 user manual's table as remembered; the Harris data sheet extract in hand has no table. OH1E uses only STPC/LDC/STM/CIE/CID (no timing-critical 68xx) |
| No CPU exerciser | no zexall equivalent for the 1802; unit tests + firmware runs only |
| IDL | left on any interrupt request, even with IE = 0 (1802 docs unclear) |
| FFSK | byte level, no bit timing beyond 1 byte = 8/1200 s for TFLAG; no hunt mode/sync |
| 93C06 EEPROM, MD50 PROM A4-A7 | not modelled (neither firmware uses the EEPROM) |
| 8253 GATE1 (inverted OUT0), OH1E's 455 kHz AFC measurement | gate not modelled |
| Tone outputs | only one latch bit captured at a time (`audio_capture(bit=)`, default PHI); OH1E's CTCSS DAC in the PROM socket not modelled |
| Watchdog OFF3 | not modelled |
| Handset | keys by label (`KEYS`), Finnish layout only |
