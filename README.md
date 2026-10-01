# tuple-2048

**A self-learning 2048 agent – n-tuple networks + temporal-difference learning in plain C – with a
German learning website that explains, step by step, how and why it learns.**

The program only knows the rules. It plays hundreds of thousands of games against the random tile
spawns and improves its evaluation after every game. No hand-written strategy, no search during
training, no external libraries.

| | |
|---|---|
| **Engine** | 64-bit bitboard, 65 536-entry row lookup tables → ~36 M moves/s (one core, random play) |
| **Learner** | N-tuple network (4×6 or 8×6 tuples, 8 symmetries), backward TD(0) on afterstates |
| **Extras** | optimistic initialisation (OTD), temporal coherence (TC), multi-stage weights, Hogwild threads, expectimax for evaluation |
| **Website** | learning curve, replay viewer ("beginner vs. pro"), learned patterns, tuple lens, *live TD learning in the browser*, line-by-line code walkthrough |

<!-- RESULTS -->

## Quick start

```bash
make                       # builds ./t2048 (C11, gcc or clang, Linux/macOS)
make test                  # unit tests
./t2048 bench              # how fast is this machine?

# a few minutes: small network, learns to reach 2048 in ~80 % of the games
./t2048 train --net small --games 100000 --threads 4 --out runs/small

# the real thing (hours): 4x6-tuple network, optimistic start, TC learning, game phases
./t2048 train --net strong --games 1000000 --threads 4 --init 40000 \
              --tc-after 500000 --stages 14 --every 10000 --out runs/showcase

./t2048 eval --weights runs/showcase/weights.bin --games 1000 --depth 1 --threads 4
./t2048 demo --weights runs/showcase/weights.bin --delay 50       # watch one game as JSON lines

make site                  # bundle runs/ into web/data/*.js (see web/bundle.json)
open web/index.html        # works straight from the file system
```

## The website

`web/index.html` (also deployable to GitHub Pages as is) – all explanations in German, aimed at
interested non-programmers:

1. **Das Spiel** – play yourself, ask the trained agent for its evaluation of your position.
2. **Wie „sieht“ der Computer ein Feld?** – the tuple lens: hover each of the 32 "experts"
   (4 tuples × 8 symmetries) and see its weight add up to the board value.
3. **Wie lernt er?** – a JavaScript port of the same learner trains *in your browser*; every few
   hundred milliseconds one real TD update is shown with numbers.
4. **Lernkurve** – average/best score and tile rates over training (log scale).
5. **Replay** – the same demo seed played after 1, 10, 100, … training games, with the q-value of
   every direction and a plain-language "why this move".
6. **Gedächtnis** – the highest-valued and most-visited patterns of every tuple.
7. **Was bringt welcher Trick?** – A/B runs with identical seeds, and search vs. no search.

`web/code.html` – every C module, section by section, with clickable line notes. The text lives in
the sources themselves (`/*@ … */`, `//@ …`) and is extracted by `tools/gen_codepage.py`, so it
cannot drift from the code (CI checks it).

## Command line

```
t2048 train [--net small|strong|strong8] [--games N] [--threads T] [--alpha A] [--init V]
            [--tc] [--tc-after G] [--tc-alpha A] [--stages E1,E2,..] [--every N]
            [--seed S] [--demo-seed S] [--demo-depth D] [--eval N] [--top-k K]
            [--min-visits V] [--snapshot-weights] [--resume weights.bin] --out DIR
t2048 demo  --weights FILE [--seed S] [--depth D] [--out FILE.jsonl] [--delay MS]
t2048 eval  --weights FILE [--games N] [--depth D] [--threads T] [--seed S]
t2048 top   --weights FILE [--k K] [--min-visits V] [--stage S] --out FILE.json
t2048 bench [--seconds S]
```

* `--init V` – optimistic initialisation: every board starts with estimated value `V`.
* `--tc-after G` – switch on TC learning (per-weight adaptive rate, `--tc-alpha`, default 1.0) after `G` games.
* `--stages 14` – separate weight tables once a 16384 tile (2¹⁴) is on the board; promoted from the previous stage on first use.
* `--resume` – continue from saved weights; `--games` is the *total* target, the CSV is appended.

## Output files (`runs/<name>/`)

| File | Content |
|---|---|
| `meta.json` | configuration, tuple shapes, git revision |
| `milestones.csv` | one row per milestone: `games, elapsed_s, games_per_s, window, avg_score, max_score, avg_moves, max_tile, rate_2048 … rate_32768, best_tile_ever` (statistics over the games since the previous row) |
| `eval.csv` | same columns, frozen-network evaluation games (`--eval N`) |
| `top/top_<games>.json` | per tuple: cells, top-k entries by value (with ≥ `min_visits` visits) and by visit count |
| `replays/replay_<games>.jsonl` | demo game with frozen weights at each snapshot (1-2-5 series): header line, one line per move (`board`, `dir`, `reward`, `score`, `after`, `spawn [pos, exp]`, `q [L,R,U,D]`), end line |
| `weights.bin` | network (overwritten at every snapshot; `--snapshot-weights` keeps all) |

Boards are 16 hex digits: digit *i* is the exponent of cell *i* (row-major from top-left, `0` = empty, `b` = 2048).

## How it learns (short version)

* **Afterstates.** The value function `V` rates the board *after sliding, before the random tile*.
  Choosing a move is then deterministic: `argmax_a r(a) + V(after(a))`.
* **N-tuple network.** `V(s)` is the sum of table lookups: each tuple reads its cells' exponents,
  concatenates them to an index and contributes one weight; every tuple is applied in all
  8 symmetric orientations sharing one table.
* **Backward TD(0).** After each game, walk the afterstates from last to first: target for the last
  is 0, for every earlier one `r_next + V(after_next)` (with the just-updated value). Each weight
  moves by `α·δ / (#features)`. Propagating backwards spreads end-of-game information through the
  whole game in a single pass.
* **OTD → TC.** Optimistic initial values drive exploration early on; TC learning later gives each
  weight its own step size `α·|E|/A` so converged weights stop jittering.

## Pitfalls we hit or avoided

1. Learn on **afterstates**, not states – otherwise every move choice needs an expectation over all spawns.
2. **Terminal update**: the last afterstate must be pulled towards 0, or end positions stay overrated forever.
3. **Reward = merge score of that move**, never the cumulative score.
4. **Invalid moves** (board unchanged) must be excluded explicitly.
5. **Learning rate per weight** = α / number of features; with symmetric boards the same weight can be hit
   several times per update – intended, but tests must not assume exact `α·δ` steps then.
6. **Engine speed ≠ training speed.** The engine does ~36 M moves/s; training is bound by random memory
   access into 268 MB of tables. `mmap` + `MADV_HUGEPAGE` helps; Hogwild threads scale well.
7. **"Top" weights are noisy**: rarely visited entries carry extreme values – filter by visit count.
8. **Optimistic init needs the right scale.** Too high (here: ≥ 120 000) and the agent spends the whole
   budget "disappointing" itself; 40 000 helped within a few hundred thousand games.
9. **Two 32768 tiles cannot merge** in a 4-bit encoding – capped explicitly in C, Python and JS alike.
10. **Same seed, same game?** Only with one thread. Demo games use a fixed seed, but the spawn positions
    depend on the board, so games diverge as soon as the agent plays differently – which is exactly the point.

## References

* Szubert, Jaśkowski: *Temporal Difference Learning of N-Tuple Networks for the Game 2048*, IEEE CIG 2014.
* Yeh, Wu, Kao, Chen: *Multi-Stage Temporal Difference Learning for 2048-like Games*, IEEE TCIAIG 2016.
* Matsuzaki: *Systematic Selection of N-Tuple Networks with Consideration of Interinfluence for Game 2048*, TAAI 2016.
* Jaśkowski: *Mastering 2048 with Delayed Temporal Coherence Learning, Multi-Stage Weight Promotion, Redundant Encoding and Carousel Shaping*, IEEE ToG 2018.
* Guei, Chen, Chen, Wu: *Optimistic Temporal Difference Learning for 2048*, IEEE ToG 2021.

## License

MIT
