"""Plays a bot against an opponent on the real engine, in the judge's sandbox.

    python -m export.evaluate --bot bots/rl_bot_v2                 # vs the C++ random starter
    python -m export.evaluate --bot bots/rl_bot_v2 --opponent bots/other --generated 12

Each map is played twice, the bot as team A and as team B, with the judge's
engine (unswbc_engine.wasm), the judge's clang and the judge's metered
sandbox, exactly as `unswbc run --sandbox` would. It reports wins, losses
("upsets" when the opponent is the random starter), draws, bot errors (a
turn that timed out, crashed or wrote nothing) and points per turn.
"""
from __future__ import annotations

import argparse
import pathlib
import random
import subprocess
import tempfile
import time
from collections import Counter
from dataclasses import dataclass, field

import numpy as np

import bccore


@dataclass
class GameRecord:
    map_name: str
    side: str
    seed: int
    winner: str | None
    rounds: int
    reason: str
    errors: list = field(default_factory=list)
    deaths: Counter = field(default_factory=Counter)

    @property
    def outcome(self) -> str:
        if self.winner is None:
            return "draw"
        return "win" if self.winner == self.side else "loss"


def random_starter(folder: pathlib.Path) -> pathlib.Path:
    """The toolkit's C++ starter bot (`unswbc init cpp`): random safe steps."""
    bot = folder / "starter"
    subprocess.run(["unswbc", "init", "cpp", str(bot)], check=True, capture_output=True)
    return bot


def maps(official: bool, generated: int, seed: int) -> list[tuple[str, str]]:
    out = []
    if official:
        import unswbc

        folder = pathlib.Path(unswbc.__file__).resolve().parent / "templates" / "maps"
        out += [(p.stem, p.read_text()) for p in sorted(folder.glob("*.map"))]
    rng = random.Random(seed)
    for i in range(generated):
        symmetry = ["x", "y", "xy"][i % 3]
        w, h = rng.randint(14, 40), rng.randint(14, 40)
        text = bccore.generate_map(w, h, symmetry, seed=rng.getrandbits(64), dragons_per_team=rng.randint(1, 4),
                                   portal_pairs=rng.randint(0, 4), kelp=rng.choice([0.0, 0.05, 0.12]))
        out.append((f"generated-{symmetry}-{w}x{h}-{i}", text))
    return out


def play(engine, map_name: str, map_text: str, wasm: dict, side: str, seed: int, points: dict) -> GameRecord:
    from unswbc.sandbox import SandboxBot, WasmPool

    record = GameRecord(map_name, side, seed, None, 0, "")
    pools = {team: WasmPool([str(wasm[team])], key=f"{seed:016x}-{team.lower()}") for team in "AB"}
    live, teams = {}, {}

    def spawn(did, init):
        team = next(line.split()[1] for line in init.decode().splitlines() if line.startswith("TEAM"))
        teams[did] = team
        live[did] = SandboxBot(pools[team], init=init, name=str(did))

    def reply(did, block):
        bot = live[did]
        out = bot.ask(block)
        if bot.error is not None:
            record.errors.append(f"team {teams[did]} dragon {did}: {bot.error}")
        spent = bot.live[0] if bot.live else 0
        if spent:
            points["bot" if teams[did] == side else "opponent"].append(spent)
        return out

    def death(did, round_num, reason):
        if teams.get(did) == side:
            record.deaths[reason] += 1
        bot = live.pop(did, None)
        if bot is not None:
            bot.stop()

    try:
        result = engine.run(map_text.encode(), reply, death, spawn, lambda line: None, 0, seed)
    finally:
        for bot in live.values():
            bot.stop()
        for pool in pools.values():
            pool.close()
    record.winner = result.winner
    record.rounds = result.rounds + 1
    record.reason = "elimination" if result.end_reason == 0 else "round limit"
    return record


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--bot", required=True)
    p.add_argument("--opponent", default=None, help="a bot folder (default: the C++ random starter)")
    p.add_argument("--no-official", action="store_true")
    p.add_argument("--generated", type=int, default=6)
    p.add_argument("--seed", type=int, default=1)
    args = p.parse_args()

    from unswbc import clangtool
    from unswbc.engine import EngineModule

    with tempfile.TemporaryDirectory() as tmp:
        opponent = pathlib.Path(args.opponent) if args.opponent else random_starter(pathlib.Path(tmp))
        name = "random starter" if args.opponent is None else opponent.name
        wasm_bot = clangtool.build(pathlib.Path(args.bot))
        wasm_opponent = clangtool.build(opponent)
        engine = EngineModule()
        points = {"bot": [], "opponent": []}
        records = []
        rng = random.Random(args.seed)
        start = time.time()
        for map_name, text in maps(not args.no_official, args.generated, args.seed):
            seed = rng.getrandbits(64)
            for side in "AB":
                wasm = {side: wasm_bot, "B" if side == "A" else "A": wasm_opponent}
                record = play(engine, map_name, text, wasm, side, seed, points)
                records.append(record)
                errors = f"  ERRORS: {record.errors[:2]}" if record.errors else ""
                deaths = ", ".join(f"{k}x{v}" for k, v in sorted(record.deaths.items()))
                print(f"{map_name:28s} as {side}: {record.outcome:4s} after {record.rounds:3d} rounds "
                      f"({record.reason}; our deaths {deaths or 'none'}){errors}", flush=True)

    tally = Counter(r.outcome for r in records)
    errors = sum(len(r.errors) for r in records)
    print(f"\nvs {name}: {tally['win']} wins, {tally['loss']} losses, {tally['draw']} draws in {len(records)} games; "
          f"{errors} bot errors; {(time.time() - start) / 60:.1f} min")
    for who, values in points.items():
        if values:
            print(f"  {who} points per turn: p50 {int(np.percentile(values, 50)):,} "
                  f"p99 {int(np.percentile(values, 99)):,} max {max(values):,}")
    losses = [f"{r.map_name} as {r.side}" for r in records if r.outcome == "loss"]
    if losses:
        print("  losses: " + "; ".join(losses))


if __name__ == "__main__":
    main()
