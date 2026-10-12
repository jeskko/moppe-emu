# moppe-emu

Emulator for Mobira/Nokia radios. The firmware and its scenario tests are in the firmware repo (`../moppe`, which has this repo as its `emu/` submodule).

- Orient from `README.md` → Status and `notes/emulator.md`.
- Notes: `X.md` holds current state; session narrative goes in `X-history.md`.
- Verify claims against listings or live runs before recording them as confirmed.
- Any emulator change: run `make test`, and from the firmware repo its test suite (`python3 tools/ci/runtests.py`) and `tools/r58/emuoracle.py` against the old library (bit-identical behaviour unless the change is meant to alter it); for `md5x.c`, `tools/md5x/digest.py` on the old and new libmd5x.so.
- Commit validated progress without asking.
