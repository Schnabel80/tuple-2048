# CLAUDE.md

Guidance for AI coding agents working on this repository.

## What this is

A self-learning 2048 agent (n-tuple network + TD learning, plain C11, no dependencies)
plus a static, German-language learning website in `web/` that explains how it works.

## Commands

```bash
make                 # build ./t2048 (-O3 -march=native -flto)
make OPT=-O2         # portable build (what CI uses)
make test            # C unit tests + code page check + Python tool tests
make bench           # engine / evaluation / training speed
make train-small     # quick sanity run (small net)
make train GAMES=…   # strong net with OTD + TC + multi-stage
make site            # regenerate web/data/code.js and web/data/runs.js
```

Run `make test` after every change. CI runs build + tests with gcc and clang plus a
2000-game smoke training run.

## Layout

- `src/` – C sources. `board` (bitboard engine) → `ntuple` (value function) →
  `td` (move choice + learning) → `search` (expectimax, play only) → `train`
  (threads, milestones) → `io` (output files) → `main` (CLI).
- `tests/test_main.c` – C tests (reference implementation for moves, symmetry,
  TD/TC updates, save/load, determinism). `tests/test_tools.py` – Python tools.
- `tools/gen_codepage.py` – builds the code walkthrough from source annotations.
- `tools/bundle.py` – turns `runs/*` into `web/data/runs.js` (config: `web/bundle.json`).
- `tools/showcase.py` – evaluates the main run with/without expectimax.
- `web/` – static site, must work from `file://` (no `fetch`, no ES modules, no CDN).

## Conventions that are easy to break

1. **Source annotations are the documentation of the code page.**
   `/*@ ### Heading … */` starts a section (German Markdown), `//@ text` is a line
   note. Every file starts with a `/*@ ## … */` overview, every later section needs a
   `###` heading. After touching `src/`, run `python3 tools/gen_codepage.py` and commit
   `web/data/code.js` – `make test` fails if it is stale. New source files must be
   added to `MODULES` in `tools/gen_codepage.py`.
2. **Generated files are committed**: `web/data/code.js`, `web/data/runs.js`.
   `runs/` itself is gitignored (weights are 0.5–2 GB).
3. **Board encoding is shared by C, Python and JS**: 16 nibbles, cell `i` = bits
   `4i..4i+3`, row-major from top-left; in files a board is 16 hex digits, digit `i`
   = exponent of cell `i`. Directions are `L R U D` in that order (q arrays use it).
   Symmetry `s`: mirror columns if `s >= 4`, then rotate 90° `(s mod 4)` times –
   implemented identically in `ntuple.c`, `tests/test_main.c` and `web/js/engine.js`.
4. **Weights file format** (`net_save`/`net_load`, read by `bundle.py`): magic,
   version 2, endian marker `0x01020304`, flags (bit 0 visits, bit 1 TC), tuple cells,
   stages, then per stage per tuple `float32 w[16^len]`, `uint32 visits[16^len]` and – if
   TC is on – `float32 E[16^len]`, `float32 A[16^len]`. Version 1 files (no TC) still load. Bump `WEIGHTS_VERSION`
   on any change and update `tools/bundle.py:read_weights`.
5. **Replay / top / CSV formats** carry `format_version` (`io.h: FORMAT_VERSION`).
   `bundle.py` compacts replays (start board + direction string + spawn string); the
   browser re-simulates them and verifies against checkpoint boards every 250 moves.
6. Training with `--threads 1` is bit-for-bit reproducible; with more threads it is
   Hogwild (lock-free shared weights) and only statistically reproducible.
7. The website is German (explanations); code, comments outside `/*@`/`//@`, and
   commit messages are English.
