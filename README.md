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
  at about 30k decisions per second end to end (1024 games, 64-step
  rollouts): PyTorch, not the environment, is the bottleneck.
- **Bot.** The export gate holds the bot to the trained actor on every
  recorded turn, built natively and by the judge's clang in the judge's
  metered sandbox: same observation, same action. A turn costs about 4.1M
  of the 100M points (2.5M of that is the single write), 0.6 MB of memory,
  and the zip is about 460 KB.
- **Strength.** `bots/rl_bot_v5` (v3's weights widened to the current
  observation and actions, then two runs of 350 iterations at mask level 2,
  about 23M decisions each: one with memory, the survival search and giving
  way, exported as `bots/rl_bot_v4`, then one with shed and dissolve;
  `models/rl_bot_v5.pt`) in the judge's sandbox, with no bot errors:
  - beats v3 in 114 of 162 games head to head (+150 Elo, 95% interval
    +95 to +214; the SPRT for +30 accepts) and v4 in 41 of 54 (+200);
  - beats the toolkit's random starter in all 108 games on the 15 official
    maps and 12 generated ones, both sides, two seeds;
  - beats the careful scripted player in 48 of 54 (v4: 39, v3: 43) and
    over|yonder's tactics-bot in 49 of 54, drawing 2 (v4: 49, no draws).

  Its queen dies in 14 of the 108 games against the starter (v3: 28), 12
  of them by round 9 on the three maps where each queen starts in a dead
  end. `bots/rl_bot_v4`, `bots/rl_bot_v3`, `bots/rl_bot_v2` and the earlier
  NumPy `bots/rl_bot_v1` are kept as baselines.

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
# Train (CPU or CUDA; checkpoints are written atomically). --init-from starts
# from an older checkpoint even across observation changes; --resume continues one:
python -m rl.train --games 1024 --rollout 64 --iterations 2000 --official-maps 0.3 \
    --kelp 0.0 0.25 --scripted-frac 0.35 --scripted-careful 0.6 \
    --checkpoint-dir checkpoints/run1 --log-csv checkpoints/run1/log.csv

# models/rl_bot_v5.pt is the checkpoint bots/rl_bot_v5 was exported from (run
# with the flags above for 350 iterations, --init-from v4's checkpoint): actor,
# critic, optimizer, league and level replay. Continue it with the same flags
# plus --resume models/rl_bot_v5.pt and a larger --iterations (keep --games
# 1024: changing it across a resume is untested). models/rl_bot_v3.pt is v3's.

# Compare checkpoints quickly, natively, against the careful scripted player:
python -m rl.eval_env checkpoints/run1/ckpt_000500.pt checkpoints/run1/latest.pt --opponent careful

# Export the actor as a C++ bot and run the export gate (native + judge sandbox):
python -m export.export_bot --checkpoint checkpoints/run1/latest.pt --out bots/rl_bot_v6

# Play it in the judge's sandbox, both sides of the official and generated maps,
# against the random starter, the careful player, or another bot:
python -m export.evaluate --bot bots/rl_bot_v6 --generated 12
python -m export.evaluate --bot bots/rl_bot_v6 --opponent careful --generated 12
# Or decide whether a new version beats an old one (SPRT, Elo bounds 0 and 30):
python -m export.evaluate --bot bots/rl_bot_v6 --opponent bots/rl_bot_v5 --sprt 0 30

# The toolkit's own commands work too:
unswbc run maps/arena.map bots/rl_bot_v5 bots/rl_bot_v5 --sandbox -v
unswbc submit bots/rl_bot_v5 -n rl-v5 -d "PPO self-play, C++ int8, queen shield"
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
View and the dragon's **memory**: each dragon is a process of its own for as
long as it lives, so it remembers its own body in board coordinates (what it
sees of it back from its head, then what it remembered from there on). A body
trails its head's path, so after L moves it knows all of it, far outside its
window. From those:

- an egocentric observation of 1245 uint8 codes: 24 planes over the 7×7
  window rotated so the dragon faces up (pearls, countdowns, kelp and
  portals by relative side, own/ally/enemy heads and bodies with their
  relative headings, and the segments of this team's queen and of the
  enemy queen), then 69 scalars (length, free sprint steps, units, round,
  queen, split legality, per-move flags: visibly fatal, pearl, enemy or
  ally head, out of sight; past each first step, how many tiles are
  reachable within the window and whether that region goes on out of
  sight, so pockets that would trap the dragon show up; for where each move
  ends and for the head's tile now, whether a visible enemy head could
  reach it before this dragon moves again; and, after each single step, how
  many moves it can be sure of surviving: a depth-first search over its
  own future steps, up to its length plus 2 (8 to 24), with other dragons
  frozen and its own body, known from memory, freeing as its tail moves;
  whether its head is within two steps of its queen's head; and how close
  to an enemy each move ends, in the steps an enemy would need to get
  there, counting the children enemies could split off: a split turns the
  parent's tail into the child's head, and the child moves in the round it
  is born, so every enemy tail can strike like a head);
- 15 relative actions: one step forward/right/left, two steps (each
  forward/right/left, free from length 5), splitting off the rear half,
  shedding a two-segment tail (`SPLIT 2`), and dissolving (no action: the
  dragon dies and every second segment becomes a pearl);
- masks: level 0 only removes an illegal split; level 1 also removes moves
  the dragon can see are certain death (kelp, bodies, through portals whose
  far end is in sight, its own tail when it knows the tail moves on, a
  second step it cannot pay for, an ally's head, and dissolving anywhere
  but within two steps of its queen's head, by a dragon of length 3 or
  more; the queen never dissolves); level 2, the default, also shields the
  queen, which decides the first round-limit tiebreak: it never moves onto
  a head; of its other moves it keeps those it can be sure of surviving its
  horizon after, else those that last longest (a move through a portal
  whose far end it cannot see counts as lasting one move), and of those
  the ones that end out of every enemy's reach on its next turn (enemy
  tails included), else out of one step's; it may shed from length 6
  before round 350 while no enemy is a step away, and splits in half only
  when it has no move left. Every other dragon keeps out of its queen's
  way: it does not end a move beside the queen's head while it has another
  move. Each level falls back to the one below if it would leave nothing,
  and a dragon with only fatal moves still avoids taking an ally with it.

The bot compiles these two headers unchanged, and keeps one memory. Training builds the View
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
| `test_core_contract.py` | training's View = the bot's View, and a dragon's memory and features fed either way agree, on every turn; the remembered body is never wrong; masks are sound (level 2 only narrows); rotation, decoding, queen planes, enemy reach, survival, giving way |
| `test_env.py` | every decision saw its turn's board; children in their birth round; shaping telescopes; horizon cuts and bootstraps; threads don't change games; queen deaths, splits and deciders are reported |
| `test_ppo.py` | rows complete once; vectorised GAE = a plain reference |
| `test_warm_start.py` | an actor or critic widened to a newer observation computes what it did before |
| `test_quantize.py` | the integer actor = the QAT float actor |
| `test_export.py` | the compiled bot = the integer actor (natively; sandbox with `RUN_SANDBOX_TESTS=1`) |

## Limitations and next steps

- **Sonar is unused.** The View carries messages and echoes, but the
  observation and actions do not use them yet.
- **The queen still gets cornered sometimes.** Memory, the survival search
  and allies giving way cut it from 42% of games against the careful player
  to about 20% (with v3's weights; v5's queen dies in 12 of 54). What is
  left is mostly crowds: enemy and allied bodies closing in between the
  queen's turns, which a search with everyone else frozen cannot foresee.
  The memory keeps only the body; a remembered map (kelp, portals, pearls)
  and sonar between allies are the next steps. (On autarky, dilemma and
  slithery_fight each queen starts at the mouth of a dead-end corridor and
  is lost by round 9 whatever it does.)
- **Feeding is crude.** Dissolving beside the queen leaves pearls it can
  reach, but the mask only checks distance: nothing steers a dragon towards
  the queen to feed it, and the policy decides on its own when it pays.
- **The longest-dragon tiebreak.** When both queens die, the bot's habit
  of splitting into many short dragons loses the second tiebreak to
  opponents that keep one long dragon (most of v3's losses to the careful
  player).
- **The policy is an MLP.** A small convolutional or attention trunk over
  the window would likely learn faster; the points budget leaves room for
  a much bigger network (a turn uses about 4% of it).
- **Two split sizes** (the rear half, a two-segment tail) and two-step sprints at most.
- **Settings chosen before measuring.** Mask level, the shaping scale and the
  level distribution deserve a sweep now that training is fast; so does the
  league and PLR configuration.
- **CPU throughput.** The environment is memory-bound across many games
  (~0.2–0.3M decisions per core); compacting the board (tiles and edges)
  would help if a GPU makes the environment the bottleneck. A CUDA port
  was not attempted.

## Sparring with over|yonder's bots

`scripts/build_overyonder_bots.sh` builds the example bots from
[over|yonder's Loong Game repository](https://github.com/overyonder/the-loong-game),
whose [blog](https://github.com/overyonder/the-loong-game/tree/master/blog)
much of this design draws on: tactics-bot, roles-bot and first-bot (Nim,
written out as C) and room-c. Pass one as `--opponent` to `export.evaluate`.
They were written for the rules before Slay the Queen.

## Rules reference

`docs/rules/` mirrors every page under
[game.battlecode.au/docs](https://game.battlecode.au/docs/overview):
overview, structure, the game map, pearls, kelp and portals, vision,
movement, splitting, sonar, death, the ELO/ladder system, the CLI, the
wire protocol, execution order, timeouts (CPU-point budget and pricing),
the sandboxed standard library, and the full helper API. Start at
`docs/rules/overview.md`.
