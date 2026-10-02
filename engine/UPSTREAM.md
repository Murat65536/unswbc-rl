# Vendored engine

This directory is the rules engine of the organisers' toolkit, vendored from
[unswcpmsoc/battlecode](https://github.com/unswcpmsoc/battlecode), `engine/`,
under its MIT licence (`LICENSE`, copied from the upstream repository root).

| | |
| --- | --- |
| Upstream repository | https://github.com/unswcpmsoc/battlecode |
| Upstream commit | `eb54612f5eb81880977597eb51a89c26a2cbdcd3` (2026-09-25, "The UNSW Battlecode toolkit") |
| Upstream `VERSION` | `1.0.2` |
| Rules targeted here | `unswbc` 1.2.3 (the toolkit the judge currently runs) |

The commit that added this directory holds the upstream files byte for byte,
so `git diff <that commit> -- engine/` shows every local change.

## Local changes

### Rules: 1.0.2 to 1.2.3

The judge's 1.2.3 engine (`unswbc_engine.wasm` in the `unswbc` wheel, which
keeps its function names) was read alongside this source, and every change is
held to it by `tests/test_fidelity.py`, which plays full seeded games on both
engines and requires identical turn blocks, deaths, splits and results.

1. **Sprint cost** (`src/actions.cc`). The first ⌈L/4⌉ steps of a move are
   free, with L fixed when the move starts; 1.0.2 charged every step after
   the first.
2. **Pearl seeding** (`types.h`, `game.h`, `src/pearls.cc`). Pearls come from a
   `std::mt19937_64` seeded with the match's 64-bit seed (`Game`'s new `seed`
   argument), drawn as `min + rng() % (max - min + 1)` in 64 bits. 1.0.2 used a
   `std::mt19937` with the fixed seed `0x5eed5eed`. The standard fully
   specifies `mt19937_64`, so any standard library reproduces the judge's
   pearls; no `uniform_int_distribution` is involved.
3. **Tiebreak** (`src/scoring.cc`, `TeamStanding::mQueenLength`). At the round
   limit the longer queen (dragon 0 or 1, length 0 once dead) wins first, then
   the longest dragon, then total length. 1.0.2 had no queen.

### Build and API

4. **Cap'n Proto is optional** (`CMakeLists.txt`, `game.h`). Replay writing
   (`src/replay.cc`, `Game::WriteReplay`) is built only with
   `-DUNSWBC_ENGINE_REPLAY=ON`; the default build has no dependencies.
   `DebugOutput::mRecord = false` stops `Game` keeping every event.
5. **A turn at a time** (`game.h`, `src/game.cc`). `Begin()`,
   `CurrentDragon()` and `TakeTurnWith(reply)` play the game one turn per call
   without controllers, for the training environment. `Run()` is now built on
   the same cursor, so both paths share one round loop.
