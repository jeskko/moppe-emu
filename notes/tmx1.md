# Nokia TMF-1 / TMN-1 (Talkman 520 / 620)

Status (2026-10-02): runs OH5NXO / OH3NWQ `tmx1.asm` v5.0 (TMF-1 and
TMN-1 builds) together with the HSN-2 v1.6 or HSF-2 v0.2 handset firmware,
each on its own emulated uPD7810, linked by a bit-level MBUS. All four
builds are byte-identical to the released binaries. Hardware facts and
sources: the firmware repo's `notes/tmx1.md`.

## Layers

| Layer | Files | Notes |
|---|---|---|
| CPU | `upd7810.c` | uPD7810 / uPD78C10: the whole instruction set (989 forms of as7810's grammar decode to as7810's text and length: `tests/tmx1/test_encodings.py`), skips with the data sheet's skipped-instruction states, the MVI A / LXI H string effect (L1/L0), interrupts (shared vectors, EI delay, HLT, NMI), ports A/B/C with mode registers and the PC control functions, timers 0/1 and the timer F/F, the timer/event counter, the asynchronous serial interface from TO, the A/D converter (scan and select), AN4-AN7 edge flags, internal RAM (MM RAE). Unit test `tests/unit/test_upd7810.c` |
| Radio | `tmx1.c` | NP5SA memory map and paging, serial bus (PLLs, DAC, LFU), 74259, MAS7825 at byte level, i8253 (`pit.c`), watchdog, power logic, the MBUS line, a passive MBUS monitor and a host MBUS transmitter |
| Handset | `tmx1hs.c` | HSN-2: 74259 pair over the EPROM (keypad rails, LCD /CS and C/D, LEDs), PCD3312 on I2C; HSF-2: keypad rails on PA, HSIC registers 0400-0700; both: two uPD7228 LCD controllers, keypad edges on AN4-AN7, POWERSW, hook |
| API | `tmx1_api.c` → `libtmx1.so`; `python/tmx1emu.py`; `python/upd7810dis.py` (as7810-syntax disassembler); `python/tmx1tui.py` | `Radio(rom, lst, hs_rom=, handset="HSN2"/"HSF2")`; `handset=None`: the host plays the handset over MBUS |
| Tests | `tests/tmx1/` (`make test-tmx1`) | `roms.py` (sources via `make refs`) builds the four images with as7810 from `tmx1_v50.zip`; `test_tmx1.py` (16 scenarios); `test_encodings.py` assembles every instruction form of as7810's grammar and checks the disassembler's text and the core's lengths against it |

Both units run at 11.0592 MHz. Time is radio clocks; the scheduler steps
whichever CPU is behind, so the two stay within one instruction. The
MBUS line is the wired AND of the radio TxD, the handset TxD and the
host transmitter; each unit's RxD and CI (PC5) see it, as the service
manual describes ("RXDATA is also connected to PC5"). Every unit hears
its own transmissions.

## Model

| Item | Model | Source |
|---|---|---|
| Instruction encodings, states | 78C10A data sheet table (1 state = 3 clocks); skipped: 4/7/8/10/11/14 states by length and the "*" marks | data sheet; as7810 cross-check |
| Interrupt mask / flags | MKL bit n = request flag n (1 FT0, 2 FT1, 3 F1, 4 F2, 5 FE0, 6 FE1, 7 FEIN), MKH 0 FAD, 1 FSR, 2 FST; on a shared vector the flag is cleared on entry only when the other source is masked | firmware: `ani MKL, ~0x24 ! INTE0, INTT1`, `ani MKH, ~0x07` (radio) and `~0x06 ! INTST, INTSR` (handset); its comment "flags are not tested when entrypoints have only one unmasked source" and its SKITs on the serial vector |
| Interrupt entry | PSW, PC pushed (as SOFTI), IE and SK cleared, 13 states | SOFTI in the data sheet; entry states assumed |
| EI | takes effect after the next instruction | assumed (the handlers end `ei; reti`) |
| TMM | bits 3..2 T0 clock, 6..5 T1 clock (00 f/12, 01 f/384, 10 TI / T0, 11 stop), 1..0 F/F input (00 T0, 01 T1); a timer matches after TMn counts | firmware: TMM 0x20 "TMR0 from f/12 into F/F, TMR1 from f/384", TM0 3 = 9600 bd, TM1 255 = 112 Hz |
| Serial | SML: clock factor (bits 1..0, 10 = x16), length (3..2, 11 = 8), PEN 4, EP 5, stop 7..6; SMH: clock source 1..0 (00 = TO rising edges), RXE 2, TXE 3; FST when TXB moves to the shifter; ER on parity, framing or overrun | firmware: SML 0x5E "1 stop, odd parity, 8 bits, x16", SMH 0x0C "rx enable, tx enable, internal TO clock" |
| Event counter | ETMM 1..0 = 00 counts f/12, 3..2 = 10 clears at CI falling edges; INTFE0/1 when ECNT equals ETM0/1 (free-running otherwise) | firmware: ETMM 0x08 "clear ECNT at CI falling edge, internal f/12 clock", ETM0 1843 = 2 ms bus-free |
| A/D | ANM bit 0 select mode, 3..1 channel (scan: bit 3 picks AN4-7), bit 4 FR; 192 states a conversion (144 with FR), INTFAD after every 4 results | firmware ANM use; conversion time from the 7810 data sheet's 48 us at 12 MHz |
| AN4-AN7 | edge flags set by falling edges (SKIT / SKNIT) | databook ("falling edge detection") |
| Radio memory | 0000-7FFF EPROM, 8000-BFFF EPROM page (PC3 PAGE = 1: C000-FFFF), C000 MAS7825 (A0), C400 i8253, D400 74259 (A2..A0, D0), E000-FFFF RAM (battery-backed, kept over power cycles) | service manual 8-5; firmware |
| Serial bus | bits on SCL (PA0) rising with SDA (PA1); RSEN / TSEN rising latch the RX / TX PLL (control bit 1: 14-bit R, else 18-bit N*128+A), LFUSTB latches the last 8 bits into MAS7845 register bits 7..6; MC144111: SCL falling while DADIS (PC4) low, latched on DADIS rising, 4 x 6 bits | firmware `synth_*`, `update_LFU_one`, `DAC_*` |
| PLL frequency | D x 12.8 MHz / R; RX PLL = receive + IF (TMF-1 build: 433.500 MHz on 454.900) | firmware `ponder_about_freq` ("12800 / 512 gives 25 kHz") |
| i8253 | CLK0 455 kHz (`set_if_hz`), CLK1 and CLK2 921.6 kHz (CLK2 after the documented tone mod); gates high; OUT2 edges captured | firmware i8253 comments |
| MAS7825 | byte level: data writes are events, TFLAG back after 8/1200 s, TXE while sending, RFLAG on `modem_rx` | firmware `modem_*`; OH5NXO MAS.registers |
| /PTT | PC2 (I2DA, 10k pull-up), low = pressed | firmware ("I2DA is our /PTT") |
| A/D defaults | BATT 200 (13.8 V), TEMP 153 (+20 C), BTMP 128, TIMEOUT 255, others 0 | service manual 8-2 figures |
| Watchdog | NMI 400 ms after the last WDC (PC6) rising edge, power off 12 s after the NMI if no pulse came | service manual 8-6 |
| Power | PWROFF (PC7) rising switches the unit (and the handset it feeds) off; the handset power key switches it on | service manual 8-6, firmware `do_quit` |
| Handset clock | 11.0592 MHz | same MBUS timer settings as the radio |
| uPD7228 | serial MSB first on SCK rising while /CS low; the first byte after /CS falls is the chip address (0 or 3); C/D = 1 command. Commands 0x64-0x6F write / OR / AND mode, 0x70-0x73 character mode, low 2 bits the pointer step (01 down, 10 up), 0x80+n data pointer, 0x08/09 display off/on. Character mode writes 5 columns and keeps bit 7 (the icon row) | inferred from hsn2.asm and hsf2.asm, which use opposite pointer directions; no data sheet |
| LCD text | decoded from the character codes written, at the cells each firmware's cursor logic uses (HSN-2: 49, 44 .. 14 middle, 9 / 4 top, 39 .. 4 bottom; HSF-2: 0 5 10 45 15 20 25 40 middle, 30 / 35 top, 0 .. 35 bottom) | the firmwares' `lcd_*_row` routines |
| Keypad | key s (1..28) closes rail (s-1)/4 onto AN4+(s-1)%4; a column falls (sets its flag) when a closed key's rail goes low | hsn2.asm "trailing edge rises ANx if contact closed"; key names from its `scancodes` |
| HSN-2 LEDs | latch D1: 2 ON, 3 SERV (lit at 0), 4 ROAM (ON-AIR), 5 CALL, 6 HF, 7 MFT; D0 bit 7 BACKLIGHT | hsn2.asm `led_chars`, "SERV funnily wired" |
| HSF-2 LEDs | HSIC register 3 bits | hsf2.asm |
| DTMF | HSN-2: the byte after the PCD3312 address; HSF-2: not reported (HSIC register 2 holds it) | firmware tables |

## Fidelity evidence

- First boot as the firmware intends: handset version, "Memories /
  Settings insane" (defaults restored), "TMF1 5.0 18.06.06 99", then the
  main screen; a second boot with the same RAM skips the reset.
- The firmware clock (`seconds`, from 112 systick interrupts) advances 61
  s in 60 s: TM1 = 255 at f/384 gives 112.94 Hz.
- MBUS bytes 1.145 ms apart within a packet (11 bits at 9600 bd); every
  byte of a boot and of key traffic passes the monitor's odd-parity and
  stop-bit check, both directions.
- Keypad entry (433550 #) moves the RX PLL to 454.950 MHz, on both
  handsets and with the host sending the key packets.
- RSSI above the squelch opens the LFU speaker path and reports "87"
  (the firmware's (200 - 30) * 100 / 195); PTT loads the TX PLL with
  433.500 MHz before TXON; CTCSS 123 Hz comes out of 8253 OUT2 at
  123.01 Hz (921.6 kHz / 7492).
- A hung main loop (DI; JR .) draws the watchdog NMI 400 ms later.

## Known limits

| Item | State |
|---|---|
| Mode-register bits not set by these firmwares | modelled by analogy (other timer clocks, serial factors, ETMM modes) or ignored: CO0/CO1 outputs (EOM), zero-cross, synchronous / I/O serial modes, timer cascade bit 7, STOP mode wake-up |
| String effect, DAA, DIV by zero, 16-bit HC | from general uPD7810 knowledge, not the docs in hand; the firmware has no consecutive MVI A / LXI H |
| uPD7228 | no character ROM (glyph bits 0..6 stay clear); blinking, multiplex, frame commands ignored; BUSY always ready |
| i8253 GATE0 (= inverted OUT1, the AFC "Center" measurement) | gates high: the AFC reading is meaningless |
| MAS7825 FFSK | byte level, no hunt/sync |
| MBUS line | ideal wired AND, no driver delays; the host transmitter waits 2 ms of free bus before a packet |
| ID EEPROM (I2C on PA0/PC2), PA3/PA7 i2c mod | not modelled (the ham firmware uses PC2 as /PTT) |
| HSN-2 NMI wake-up, handset watchdog, HSIC audio | not modelled |
| Original Nokia NMT firmware | not available; untested |
