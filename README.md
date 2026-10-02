# unswbc-rl

Reinforcement learning for [UNSW Battlecode 2026](https://game.battlecode.au/docs/overview)
("Dragons"), a Snake-like multi-agent game: dragons grow by eating pearls,
split, and die by hitting kelp, themselves or each other. The rules are
mirrored in `docs/rules/`.

One policy runs every dragon. It is trained by self-play PPO in PyTorch, on a
C++ simulator that **is** the organisers' engine (patched to the current
1.2.3 rules and checked turn by turn against the judge's own), and it is
submitted as a C++ bot that runs the same observation code and an int8
version of the same network.

```
 engine/  the organisers' rules engine (MIT), patched to 1.2.3   ─┐
 core/    the shared core: view, observation, masks, actions      ├─ C++, one source of truth
          + map generator + multithreaded batched environment     ─┘
   │ bccore (nanobind)                         │ copied into the bot unchanged
 rl/      PyTorch PPO trainer                  export/  exporter, int8 bot, export gate, evaluation
```

## Status

- **Engine fidelity.** `tests/test_fidelity.py` plays full seeded games,
  pearls included, on this engine and on the judge's real engine
  (`unswbc_engine.wasm`, pinned by hash; unswbc 1.2.3 and 1.2.4 ship the
  same one) with scripted players, and requires every init block, turn
  block, death and result to be identical: the 15 official maps, generated
  maps in all three symmetries (kelp, portals, small unit limits) and a
  hand-built map that forces each round-limit tiebreak. 214 games and about
  194,000 turns match; the test also asserts that every death reason,
  free/paid/unpayable sprints, legal, illegal and unit-limit splits,
  portals, sonar and every way a game ends came up. Reverting any of the
  three rule patches makes it fail.
- **Shared core.** The View training builds from the engine's state equals
  the View the bot parses from the protocol text on every turn tested
  (`tests/test_core_contract.py`), so the observation, masks and action
  decoding agree by construction. Masks never hide an action that could
  survive (checked by playing every masked action on a copy of the board);
  level 2 only narrows the queen's choices further (its shield).
- **Throughput.** About 3M engine turns per second on one core (32×32
  boards), and about 0.65M full decisions per second (observation, masks,
  critic features and rewards included) from the batched environment on
  this 4-core machine (`python -m rl.bench_env`). Training on CPU here runs
  at about 14k decisions per second end to end: PyTorch, not the
  environment, is the bottleneck.
- **Bot.** The export gate holds the bot to the trained actor on every
  recorded turn, built natively and by the judge's clang in the judge's
  metered sandbox: same observation, same action. A turn costs about 4M of
  the 100M points (2.5M of that is the single write), 0.6 MB of memory, and
  the zip is about 420 KB.
- **Strength.** `bots/rl_bot_v2` (about 35M training decisions, roughly an
  hour of iteration time on this 4-core CPU: 360 iterations of self-play
  with the league and level replay, then 180 more with a quarter of the
  games against the scripted random-safe player) beats the toolkit's
  random starter in 106 of 108 games on the 15 official maps and 12
  generated ones, both sides, two seeds, with no bot errors. Both losses
  are trauma as team A, a round-limit tiebreak: the bot splits into many
  short dragons and loses its queen in the maze, and the starter's single
  dragon ends up the longest. `bots/rl_bot_v1` is the earlier NumPy bot,
  kept as a baseline.

## Setup

```bash
python -m venv .venv && . .venv/bin/activate
pip install -r requirements-dev.txt      # torch, numpy, nanobind, pytest, unswbc==1.2.3

# Build the C++ core (module bccore). Either:
cmake -S . -B build -G Ninja -DPython_EXECUTABLE=$(which python) && cmake --build build
#   (drops bccore*.so in the repository root, where the tests and rl/ find it)
# or:
pip install --no-build-isolation .      # scikit-build-core

python -m pytest -q                      # ~2 minutes; FIDELITY_SCALE=5 for a longer lockstep run
RUN_SANDBOX_TESTS=1 python -m pytest tests/test_export.py   # also check a bot in the judge's sandbox
```

A C++23 compiler is needed for the core (GCC 13 or Clang 17 and up). The bot
itself is C++20, as the judge builds it.

## Train, export, evaluate, submit

```bash
# Train (CPU or CUDA; checkpoints are written atomically):
python -m rl.train --games 1024 --rollout 64 --iterations 2000 --official-maps 0.3 \
    --kelp 0.0 0.25 --scripted-frac 0.25 \
    --checkpoint-dir checkpoints/run1 --log-csv checkpoints/run1/log.csv

# Export the actor as a C++ bot and run the export gate (native + judge sandbox):
python -m export.export_bot --checkpoint checkpoints/run1/latest.pt --out bots/rl_bot_v2

# Play it against the random starter on the official and generated maps, both sides:
python -m export.evaluate --bot bots/rl_bot_v2 --generated 12
# Or decide whether a new version beats an old one (SPRT, Elo bounds 0 and 30):
python -m export.evaluate --bot bots/new --opponent bots/rl_bot_v2 --sprt 0 30

# The toolkit's own commands work too:
unswbc run maps/arena.map bots/rl_bot_v2 bots/rl_bot_v2 --sandbox -v
unswbc submit bots/rl_bot_v2 -n rl-v2 -d "PPO self-play, C++ int8"
```

## How it works

**`engine/`** is `engine/` from
[unswcpmsoc/battlecode](https://github.com/unswcpmsoc/battlecode) at the
commit recorded in `engine/UPSTREAM.md`, vendored byte for byte and then
patched: the three rule changes from 1.0.2 to 1.2.3 (sprints get ⌈L/4⌉ free
steps; pearls come from a `std::mt19937_64` seeded with the 64-bit match
seed; the queen decides round-limit tiebreaks first), Cap'n Proto made
optional, a turn-at-a-time API, and speed (an occupancy grid, no events
nobody listens to, pearl beds listed once). `UPSTREAM.md` lists every change.
Seeded with the match seed, a game here is the judge's game, pearls and all.

**`core/include/core/view.h`** is what a dragon knows on its turn, and a
reader for the wire protocol. **`features.h`** computes everything else from a
View only:

- an egocentric observation of 1227 uint8 codes: 24 planes over the 7×7
  window rotated so the dragon faces up (pearls, countdowns, kelp and
  portals by relative side, own/ally/enemy heads and bodies with their
  relative headings, and the segments of this team's queen and of the
  enemy queen), then 51 scalars (length, free sprint steps, units, round,
  queen, split legality, per-move flags: visibly fatal, pearl, enemy or
  ally head, out of sight; past each first step, how many tiles are
  reachable within the window and whether that region goes on out of
  sight, so pockets that would trap the dragon show up; and, for where
  each move ends and for the head's tile now, whether a visible enemy head
  could reach it before this dragon moves again);
- 13 relative actions: one step forward/right/left, two steps (each
  forward/right/left, free from length 5), and splitting off the rear half;
- masks: level 0 only removes an illegal split; level 1 also removes moves
  the dragon can see are certain death (kelp, bodies, through portals whose
  far end is in sight, its own tail when it knows the tail moves on, a
  second step it cannot pay for, an ally's head); level 2, the default,
  also shields the queen, which decides the first round-limit tiebreak: it
  never moves onto a head, keeps to open water (regions that go on out of
  sight, not closed ones, which trap it sooner or later) and out of one
  step's reach of enemy heads while it has a move that does, and splits
  only when it has no move left. Each level falls back to the
  one below if it would leave nothing, and a dragon with only fatal moves
  still avoids taking an ally with it.

The bot compiles these two headers unchanged. Training builds the View
straight from the engine (**`state_view.h`**), and also gives the critic
privileged team standings the actor never sees.

**`core/include/core/batch_env.h`** steps thousands of games across a thread
pool. Every game slot always has one decision outstanding (the engine's own
turn order, so every decision sees the board exactly as it is on that
dragon's turn, and a split child is asked later in its birth round), and the
next observations go straight into the trainer's tensors. A dragon's reward is
its team's result (+1/−1/0, discounted to its last decision; a dead dragon
waits for it) plus potential-based shaping on the round-limit tiebreaks in
their order (the queens' lengths, the longest dragons', the teams' totals),
which telescopes to a constant. Each finished game reports when each queen
died, how often it split and which tiebreak decided the game. A level is a 64-bit seed naming the same
generated (or official) map and match seed in any process, so levels can be
replayed. `--max-rounds` below 500 cuts games with bootstrap observations.

**`rl/`** is PPO on that environment: rows complete when the environment
says so, open rows carry into the next rollout, GAE runs along each dragon's
own decisions. The actor is an MLP on the observation codes trained
quantisation-aware (int8 weights, uint8 activations); the critic is separate
and privileged. A share of game slots pit the learner against frozen
snapshots (a league), another share against a scripted random-safe player
played inside the environment (`--scripted-frac`; self-play alone never
punished losing the queen early, since both sides did it), and Prioritized
Level Replay picks levels. The log follows the learner's queen: how often
it is alive at the end and how often it split. New observation features are
only ever appended, so `--init-from` can start a run from a checkpoint
trained on an older layout, with zero weights on the new inputs
(`rl/warm_start.py`).

**`export/`** turns a checkpoint into a bot: `quantize.py` derives the
integer actor (int8 weights, int32 accumulation, fixed-point requantisation),
`bot/net.h` runs it with WASM SIMD in the judge, and `gate.py` requires the
compiled bot to agree with training on every recorded turn, natively and in
the judge's sandbox, reporting points, memory and zip size. `evaluate.py`
plays it against the random starter (or any bot) in the sandbox, reporting
how the queens fared (when each died and how, queen splits) and what decided
each game, and with `--sprt` runs a sequential test between two versions.

## Tests

| test | what it holds |
| --- | --- |
| `test_fidelity.py` | our engine = the judge's engine, turn for turn, full seeded games |
| `test_rules.py` | individual rules on hand-built boards; the occupancy grid; generated boards = their loaded text |
| `test_core_contract.py` | training's View = the bot's View on every turn; masks are sound (level 2: only the queen's rules); rotation, decoding, queen planes, enemy reach |
| `test_env.py` | every decision saw its turn's board; children in their birth round; shaping telescopes; horizon cuts and bootstraps; threads don't change games; queen deaths, splits and deciders are reported |
| `test_ppo.py` | rows complete once; vectorised GAE = a plain reference |
| `test_warm_start.py` | an actor or critic widened to a newer observation computes what it did before |
| `test_quantize.py` | the integer actor = the QAT float actor |
| `test_export.py` | the compiled bot = the integer actor (natively; sandbox with `RUN_SANDBOX_TESTS=1`) |

## Limitations and next steps

- **Sonar is unused.** The View carries messages and echoes, but the
  observation and actions do not use them yet.
- **Queen and longest dragon.** The bot still under-values the round-limit
  tiebreak order (queen, then longest, then total): it splits freely and
  risks its queen. Shaping on the longest dragon rather than the total,
  or longer training against opponents that survive to the limit, are
  the obvious next steps.
- **The policy is an MLP.** A small convolutional or attention trunk over
  the window would likely learn faster; the points budget leaves room for
  a much bigger network (a turn uses about 4% of it).
- **One split size** (the rear half) and two-step sprints at most.
- **Settings chosen before measuring.** Mask level, the shaping scale and the
  level distribution deserve a sweep now that training is fast; so does the
  league and PLR configuration.
- **CPU throughput.** The environment is memory-bound across many games
  (~0.2–0.3M decisions per core); compacting the board (tiles and edges)
  would help if a GPU makes the environment the bottleneck. A CUDA port
  was not attempted.

## Rules reference

`docs/rules/` mirrors every page under
[game.battlecode.au/docs](https://game.battlecode.au/docs/overview):
overview, structure, the game map, pearls, kelp and portals, vision,
movement, splitting, sonar, death, the ELO/ladder system, the CLI, the
wire protocol, execution order, timeouts (CPU-point budget and pricing),
the sandboxed standard library, and the full helper API. Start at
`docs/rules/overview.md`.
