"""Plays a bot against an opponent on the real engine, in the judge's sandbox.

    python -m export.evaluate --bot bots/rl_bot_v2                 # vs the C++ random starter
    python -m export.evaluate --bot bots/rl_bot_v2 --opponent bots/other --generated 12
    python -m export.evaluate --bot bots/new --opponent bots/old --sprt 0 30

Each map is played twice, the bot as team A and as team B, with the judge's
engine (unswbc_engine.wasm), the judge's clang and the judge's metered
sandbox, exactly as `unswbc run --sandbox` would. It reports wins, losses
("upsets" when the opponent is the random starter), draws, bot errors (a
turn that timed out, crashed or wrote nothing) and points per turn.

With --sprt ELO0 ELO1 it keeps playing (the maps again, with new seeds)
until a sequential probability ratio test decides between "the bot is at
most ELO0 stronger" (H0) and "at least ELO1 stronger" (H1), at error rates
--alpha and --beta, or --max-games runs out.
"""
from __future__ import annotations

import argparse
import pathlib
import random
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


def expected_score(elo: float) -> float:
    return 1.0 / (1.0 + 10.0 ** (-elo / 400.0))


def sprt_llr(scores: list[float], elo0: float, elo1: float) -> float:
    """Log-likelihood ratio of H1 (elo1) against H0 (elo0) for game scores
    (1 win, 0.5 draw, 0 loss), in the normal approximation used by engine
    testing frameworks: N / 2 * ((m - s0)^2 - (m - s1)^2) / var."""
    n = len(scores)
    if n < 2:
        return 0.0
    mean = sum(scores) / n
    var = sum((s - mean) ** 2 for s in scores) / n
    if var <= 0:
        var = 1e-3  # all games the same result: still let the evidence count
    s0, s1 = expected_score(elo0), expected_score(elo1)
    return n / 2.0 * ((mean - s0) ** 2 - (mean - s1) ** 2) / var


def sprt_bounds(alpha: float, beta: float) -> tuple[float, float]:
    import math

    return math.log(beta / (1 - alpha)), math.log((1 - beta) / alpha)


def random_starter(folder: pathlib.Path) -> pathlib.Path:
    """The toolkit's C++ starter bot (`unswbc init cpp`): random safe steps."""
    from unswbc.init import create

    bot = folder / "starter"
    if create("cpp", str(bot), True) != 0:
        raise SystemExit("unswbc init cpp failed")
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
    p.add_argument("--sprt", type=float, nargs=2, metavar=("ELO0", "ELO1"), default=None)
    p.add_argument("--alpha", type=float, default=0.05)
    p.add_argument("--beta", type=float, default=0.05)
    p.add_argument("--max-games", type=int, default=2000)
    args = p.parse_args()

    from unswbc.engine import EngineModule

    from .gate import build_wasm

    with tempfile.TemporaryDirectory() as tmp:
        opponent = pathlib.Path(args.opponent) if args.opponent else random_starter(pathlib.Path(tmp))
        name = "random starter" if args.opponent is None else opponent.name
        wasm_bot = build_wasm(pathlib.Path(args.bot))
        wasm_opponent = build_wasm(opponent)
        engine = EngineModule()
        points = {"bot": [], "opponent": []}
        records = []
        rng = random.Random(args.seed)
        start = time.time()
        pool = maps(not args.no_official, args.generated, args.seed)
        lower, upper = sprt_bounds(args.alpha, args.beta)
        verdict = None
        passes = 0
        while verdict is None:
            for map_name, text in pool:
                seed = rng.getrandbits(64)
                for side in "AB":
                    wasm = {side: wasm_bot, "B" if side == "A" else "A": wasm_opponent}
                    record = play(engine, map_name, text, wasm, side, seed, points)
                    records.append(record)
                    errors = f"  ERRORS: {record.errors[:2]}" if record.errors else ""
                    deaths = ", ".join(f"{k}x{v}" for k, v in sorted(record.deaths.items()))
                    line = (f"{map_name:28s} as {side}: {record.outcome:4s} after {record.rounds:3d} rounds "
                            f"({record.reason}; our deaths {deaths or 'none'}){errors}")
                    if args.sprt:
                        scores = [{"win": 1.0, "draw": 0.5, "loss": 0.0}[r.outcome] for r in records]
                        llr = sprt_llr(scores, *args.sprt)
                        line += f"  LLR {llr:+.2f} [{lower:.2f}, {upper:.2f}]"
                        if llr >= upper:
                            verdict = f"H1: at least {args.sprt[1]:+g} Elo"
                        elif llr <= lower:
                            verdict = f"H0: at most {args.sprt[0]:+g} Elo"
                    print(line, flush=True)
                    if verdict or len(records) >= args.max_games:
                        break
                if verdict or len(records) >= args.max_games:
                    break
            passes += 1
            if not args.sprt or len(records) >= args.max_games:
                break
            pool = maps(not args.no_official, args.generated, args.seed + passes)

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
    if args.sprt:
        print(f"  SPRT [{args.sprt[0]:+g}, {args.sprt[1]:+g}] Elo: {verdict or 'undecided after the game limit'}")


if __name__ == "__main__":
    main()
