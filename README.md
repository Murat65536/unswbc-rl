# unswbc-rl

A from-scratch reinforcement learning attempt at [UNSW Battlecode
2026](https://game.battlecode.au/docs/overview) ("Dragons"), a Snake-like
multi-agent competition: dragons grow by eating pearls, split, and die by
colliding with kelp, themselves, or each other. Full rules are mirrored
in `docs/rules/` (scraped from the docs site) and summarized below.

This trains one shared-weight policy for every dragon via self-play PPO on
a custom Python simulator (fast enough for RL, not a wrapper around the
real engine), then exports it to a plain-NumPy bot that runs under the
actual `unswbc` judge and can be submitted to the ladder.

## Status

This is a working first attempt, not a tuned, ladder-ready bot. The
pipeline (simulator -> self-play PPO -> export -> real engine) is built,
tested, and validated end to end:

- `tests/test_engine.py` checks the simulator's rules (movement, pearls,
  kelp, wraparound, self/head-to-head collisions, tiebreaks) against
  `docs/rules/`.
- `tests/test_obs_contract.py` feeds a synthetic game state through the
  *real* `helper.py` and checks it produces the exact same observation
  vector as the training-side encoder -- the thing most likely to silently
  break a sim-trained policy on export.
- A tiny smoke-training run (1v1, no kelp, 60 PPO iterations, a few
  minutes) took mean episode length from ~2 to ~35 rounds and final
  dragon length from ~3 to ~13, and the exported bot beat the
  `unswbc init` starter bot in a real, judge-sandboxed match
  (`bots/rl_bot_v1`, trained on `bots/maps/arena.map`). That's "learning is
  working," not "ready to compete" -- see Roadmap.

## Layout

```
sim/      From-scratch game engine + map generator + observation encoder + multi-agent env
rl/       PPO (self-play, shared weights), the policy network, vectorized rollout collection
export/   Turns a checkpoint into a submittable unswbc bot (NumPy-only inference)
tests/    Engine unit tests + the sim<->bot observation contract test
notebooks/train_colab.ipynb   Colab training notebook (GPU)
bots/     unswbc bot projects: rl_bot (random starter, baseline) and rl_bot_v1 (exported demo)
docs/rules/   Rules reference, scraped from game.battlecode.au/docs/overview
```

## Quickstart

```bash
pip install -r requirements.txt   # torch + numpy, for training
pip install unswbc                # the contest toolkit (also: `uv tool install unswbc`)

# Regenerate the real helper.py used by the bot + the contract test:
unswbc init python bots/rl_bot

# Run the tests:
python -m pytest tests -q

# Train (small/fast local example -- see notebooks/train_colab.ipynb for a
# real curriculum on a Colab GPU):
python -m rl.train --num-envs 16 --team-size 2 --width 16 --height 16 \
    --max-rounds 250 --iterations 500 --rollout-length 128 \
    --checkpoint-dir checkpoints --log-csv checkpoints/log.csv

# Export the trained policy to a submittable bot:
python -m export.export_bot --checkpoint checkpoints/latest.pt --out bots/rl_bot_trained

# Try it against itself with the real engine:
unswbc run bots/maps/arena.map bots/rl_bot_trained bots/rl_bot_trained --sandbox -v

# Submit it:
unswbc auth set bc_...
unswbc submit bots/rl_bot_trained -n rl-v1 -d "PPO self-play"
```

## How it works

**`sim/engine.py`** is a from-scratch, single-game implementation of the
rules (movement incl. the sprint free-steps/payment formula, pearl
spawning incl. symmetric tile pairing, kelp, portals, wraparound,
collisions and pearl-dropping on death, round loop, and the queen-length /
longest-dragon / total-length tiebreak order), written directly off
`docs/rules/*.md`. It does not shell out to the real `unswbc` engine --
that talks to bot subprocesses over text, far too slow to drive millions
of RL steps.

**`sim/obs.py`** turns the egocentric 7x7 vision window (the *same*
information a real bot gets -- has_pearl, pearl countdown, the four edges,
and team/head-or-body for any dragon part, per tile) plus a few scalars
(own length, facing, team unit count, round number) into a flat 840-float
vector. This boundary matters: whatever the policy uses in training is all
it gets at competition time, nothing more.

**`sim/env.py`** wraps the engine in a multi-agent RL API: every living
dragon (both teams) is an independent agent sharing one policy, since
that's exactly how the real game works -- no inter-dragon shared memory,
only sonar (not yet modeled, see Roadmap), and perfectly symmetric rules
for both sides.

**`rl/`** is a fairly standard self-play PPO: `policy.py` is a small
MLP actor-critic (840 -> 256 -> 256 -> {4 action logits, 1 value}),
`vector_env.py` runs many env copies in parallel (in-process or via
`multiprocessing`), and `ppo.py`/`buffer.py` handle the bookkeeping that
variable-length, asynchronously-dying agent trajectories need (GAE per
agent, bootstrapped at rollout-length truncation, PPO clipped-surrogate
updates on the pooled batch).

**`export/`** turns a checkpoint into an `unswbc` bot project: weights
are dumped as raw float32 blobs (not `.npz` -- parsing even an
uncompressed zip through Python's `zipfile` burned enough CPU points in
the judge's sandbox to make the first turn time out; raw `np.fromfile`
doesn't), and `bot_template/main.py.tmpl` + `bot_template/bot_encoding.py`
re-implement the observation encoding and the forward pass using only
NumPy and the real `helper.py`, matching `sim/obs.py` feature-for-feature.
A small safety net masks moves that would immediately hit kelp or a
non-head body segment (a head-on-head "trade" is left unmasked -- that's
sometimes the right play, so the policy has to learn it, not have it
hidden).

## Known limitations / roadmap

This is a first attempt; these are the corners knowingly cut, roughly in
the order they're worth fixing next:

- **No splitting.** Dragons never split in the simulator or the exported
  bot. Splitting mid-round creates new agents with fresh ids part-way
  through a round, which complicates the vectorized rollout bookkeeping
  enough that it was left out of v1.
- **No sonar.** No inter-dragon communication is modeled or used.
- **Portals use our own (self-consistent) convention**, not necessarily a
  byte-exact replica of the judge's internal edge indexing -- see the
  docstring in `sim/map.py`'s `portal_destination`. Direction-preserving
  and double-sided either way, but worth re-deriving carefully (or
  checking against real replays) before relying on portal-heavy maps.
  Procedural training maps default to `portal_pairs=0` for this reason.
- **No official `.map` file parsing.** Training maps are procedurally
  generated (180-degree rotational symmetry only; `x`/`y` mirror symmetry
  isn't implemented). This doesn't block using the real maps in
  `bots/maps/` for local evaluation via `unswbc run` -- the exported bot
  talks to the real engine directly and never touches our map code -- it
  just means the policy hasn't specifically trained on them.
  `tests/test_engine.py`'s scenarios are hand-built for the same reason.
- **The sprint payment edge case** (exactly when a dragon "can't pay" and
  dies, per `docs/rules/movement.md` / `execution-order.md`) is a
  best-effort reading of possibly-ambiguous prose, and untested since v1's
  RL action space only issues single-tile moves (sprinting is implemented
  in the engine for completeness but unused by training).
- **No real vectorized simulation.** Parallelism is either a plain Python
  loop (`SerialVecEnv`) or one OS process per env (`SubprocVecEnv`);
  rounds are simulated one dragon at a time in Python. A numpy-batched
  engine (stepping all envs' dragons together) would likely be an order
  of magnitude faster and is the best lever for training a stronger
  policy without touching the RL algorithm.
- **Policy is a plain MLP**, not a CNN, despite the observation being a
  spatial 7x7 grid -- simpler to export as a hand-rolled NumPy forward
  pass. A small conv stack would likely learn faster/generalize better
  across map sizes and is a reasonable next step (conv2d-by-hand in NumPy
  is more work than the two matmuls here, but not a lot more).
- **Reward shaping is a simple, untuned default**
  (`sim/env.py`'s `default_reward_config`): +1 per pearl eaten, a small
  per-round survival bonus, a death penalty, and a terminal win/loss
  bonus. No reward for e.g. protecting the queen specifically, which the
  tiebreak rules make disproportionately important late in a game.
- **No opponent pool / league play.** Training is naive self-play against
  the current policy's own latest weights, which can cycle or
  overspecialize; keeping a pool of past checkpoints to sample opponents
  from is a standard, fairly cheap improvement.

## Rules reference

`docs/rules/` mirrors every page under
[game.battlecode.au/docs](https://game.battlecode.au/docs/overview):
overview, structure, the game map, pearls, kelp and portals, vision,
movement, splitting, sonar, death, the ELO/ladder system, the CLI, the
wire protocol, execution order, timeouts (CPU-point budget and pricing),
the sandboxed standard library, and the full helper API. Start at
`docs/rules/overview.md` if you want the rules without reading engine
code.

The `unswbc` CLI itself (toolkit for building/running/submitting bots,
bundled real maps, the replay viewer) is the other primary source this
was built from -- run `unswbc help <command>` for anything not covered
above.
