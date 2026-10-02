"""The export gate: the bot as compiled must do what the trained actor does.

1. Record turn blocks from games the actor plays itself (the 15 official
   maps when the toolkit is installed, and generated maps in every
   symmetry), with, for each turn, the observation and mask training would
   compute (bccore.features_from_blocks) and the integer actor's action.
2. Native: build the bot with the host compiler (plus -DUNSWBC_GATE, which
   makes it print its observation), feed it each dragon's blocks, and
   require the same observation and the same reply on every turn.
3. Sandbox: build it with the judge's own clang and flags, run it in the
   judge's metered wasm sandbox on the same blocks, and require the same
   reply on every turn; report points per turn (p50/p99/max) and memory.
4. The zip fits 4 MB.

It also reports how often the integer actor picks the float (QAT) actor's
action; that is informative, not a condition: the bot is held to the
integer actor exactly.
"""
from __future__ import annotations

import io
import pathlib
import random
import subprocess
import tempfile
import zipfile
from dataclasses import dataclass, field

import numpy as np
import torch

import bccore
from rl.policy import Actor, masked_logits

from .quantize import IntegerActor

MAX_TURN_POINTS = 100_000_000
MAX_MEMORY = 48 * 1024 * 1024
MAX_ZIP = 4 * 1024 * 1024


@dataclass
class Sequence:
    """One dragon's process: its init block and the round blocks it read."""
    did: int
    init: str
    blocks: list = field(default_factory=list)
    obs: list = field(default_factory=list)
    replies: list = field(default_factory=list)


@dataclass
class GateReport:
    turns: int = 0
    float_agreement: float = float("nan")
    native_turns: int = 0
    native_obs_mismatch: int = 0
    native_reply_mismatch: int = 0
    sandbox_turns: int = 0
    sandbox_reply_mismatch: int = 0
    sandbox_errors: int = 0
    points: list = field(default_factory=list)
    memory: int = 0
    zip_bytes: int = 0
    shared_core_identical: bool = False
    notes: list = field(default_factory=list)

    @property
    def passed(self) -> bool:
        ok = (self.turns > 0 and self.shared_core_identical and self.native_turns == self.turns
              and self.native_obs_mismatch == 0 and self.native_reply_mismatch == 0
              and self.zip_bytes <= MAX_ZIP)
        if self.sandbox_turns:
            ok = ok and self.sandbox_reply_mismatch == 0 and self.sandbox_errors == 0
            ok = ok and max(self.points) < MAX_TURN_POINTS // 2 and self.memory < MAX_MEMORY
        return ok

    def percentile(self, q: float) -> int:
        return int(np.percentile(self.points, q)) if self.points else 0

    def summary(self) -> str:
        lines = [f"export gate: {'PASSED' if self.passed else 'FAILED'}",
                 f"  recorded turns:        {self.turns} (integer actor = float QAT actor on "
                 f"{self.float_agreement:.2%})",
                 f"  shared core identical: {self.shared_core_identical}",
                 f"  native:  {self.native_turns} turns, {self.native_obs_mismatch} observation and "
                 f"{self.native_reply_mismatch} reply mismatches"]
        if self.sandbox_turns:
            lines.append(f"  sandbox: {self.sandbox_turns} turns, {self.sandbox_reply_mismatch} reply mismatches, "
                         f"{self.sandbox_errors} errors")
            lines.append(f"  points per turn: p50 {self.percentile(50):,}  p99 {self.percentile(99):,}  "
                         f"max {max(self.points):,} (budget {MAX_TURN_POINTS:,})")
            lines.append(f"  memory: {self.memory / 2 ** 20:.1f} MB (limit 48 MB)")
        else:
            lines.append("  sandbox: skipped")
        lines.append(f"  zip: {self.zip_bytes / 1024:.0f} KB (limit 4096 KB)")
        lines += [f"  note: {note}" for note in self.notes]
        return "\n".join(lines)


def official_map_texts() -> list[str]:
    try:
        import unswbc
    except ImportError:
        return []
    folder = pathlib.Path(unswbc.__file__).resolve().parent / "templates" / "maps"
    return [p.read_text() for p in sorted(folder.glob("*.map"))]


def gate_maps(games: int, seed: int = 0) -> list[tuple[str, int]]:
    rng = random.Random(seed)
    maps = [(text, rng.getrandbits(64)) for text in official_map_texts()]
    while len(maps) < games:
        symmetry = ["x", "y", "xy"][len(maps) % 3]
        text = bccore.generate_map(rng.randint(12, 40), rng.randint(12, 40), symmetry, seed=rng.getrandbits(64),
                                   dragons_per_team=rng.randint(1, 4), portal_pairs=rng.randint(0, 4),
                                   kelp=rng.choice([0.0, 0.05, 0.12]))
        maps.append((text, rng.getrandbits(64)))
    return maps[:games]


def record(integer: IntegerActor, mask_level: int, games: int, turns_per_game: int = 400) -> list[Sequence]:
    """Games the integer actor plays against itself, every turn recorded."""
    sequences = []
    for text, seed in gate_maps(games):
        match = bccore.Match(text, seed)
        match.begin()
        mine: dict[int, Sequence] = {}
        for _ in range(turns_per_game):
            did = match.current()
            if did is None:
                break
            if did not in mine:
                mine[did] = Sequence(did, match.init_block(did))
            seq = mine[did]
            block = match.round_block(did)
            obs, mask0, mask1 = bccore.features_from_blocks(seq.init, block)
            mask = mask1 if mask_level == 1 else mask0
            action = int(integer.act(obs[None], mask[None])[0])
            reply = bccore.decode_action(seq.init, block, action)
            seq.blocks.append(block)
            seq.obs.append((obs, mask))
            seq.replies.append(reply)
            match.reply(reply + "\nENDTURN\n")
        sequences += list(mine.values())
    return sequences


@torch.no_grad()
def float_agreement(actor: Actor, integer: IntegerActor, sequences: list[Sequence]) -> float:
    obs = np.stack([o for s in sequences for o, m in s.obs])
    mask = np.stack([m for s in sequences for o, m in s.obs])
    float_actions = masked_logits(actor(torch.from_numpy(obs)), torch.from_numpy(mask)).argmax(1).numpy()
    return float((float_actions == integer.act(obs, mask)).mean())


def native_check(bot_dir: pathlib.Path, sequences: list[Sequence], report: GateReport):
    with tempfile.TemporaryDirectory() as tmp:
        binary = pathlib.Path(tmp) / "gatebot"
        # The toolkit's own native build: -std=c++20 -O2, run from the bot's folder.
        subprocess.run(["c++", "-std=c++20", "-O2", "-DUNSWBC_GATE", "main.cpp", "-o", str(binary)],
                       cwd=bot_dir, check=True)
        for seq in sequences:
            stdin = seq.init + "".join(seq.blocks)
            out = subprocess.run([str(binary)], input=stdin.encode(), capture_output=True, check=True,
                                 cwd=bot_dir).stdout.decode()
            lines = [line for line in out.split("\n") if line and line != "ENDTURN"]
            seen_obs = [bytes.fromhex(line[4:]) for line in lines if line.startswith("OBS ")]
            replies = [line for line in lines if not line.startswith("OBS ")]
            for i, (obs_mask, expected) in enumerate(zip(seq.obs, seq.replies)):
                report.native_turns += 1
                if i >= len(seen_obs) or seen_obs[i] != obs_mask[0].tobytes():
                    report.native_obs_mismatch += 1
                if i >= len(replies) or replies[i] != expected:
                    report.native_reply_mismatch += 1


def sandbox_check(bot_dir: pathlib.Path, sequences: list[Sequence], report: GateReport, max_turns: int):
    try:
        from unswbc import clangtool
        from unswbc.sandbox import SandboxBot, WasmPool
    except ImportError:
        report.notes.append("the unswbc toolkit is not installed: sandbox check skipped")
        return
    wasm = clangtool.build(bot_dir)
    pool = WasmPool([str(wasm)], cwd=str(bot_dir), key="gate")
    try:
        for seq in sequences:
            if report.sandbox_turns >= max_turns:
                break
            bot = SandboxBot(pool, init=seq.init.encode(), name=str(seq.did))
            for block, expected in zip(seq.blocks, seq.replies):
                out = bot.ask(block.encode())
                report.sandbox_turns += 1
                if bot.error is not None:
                    report.sandbox_errors += 1
                    report.notes.append(f"dragon {seq.did}: {bot.error}")
                    break
                points, memory = bot.live
                report.points.append(int(points))
                report.memory = max(report.memory, int(memory))
                if out.decode().strip() != expected:
                    report.sandbox_reply_mismatch += 1
            bot.stop()
    finally:
        pool.close()


def zip_size(bot_dir: pathlib.Path) -> int:
    """The submission zip as `unswbc submit` builds it (the files bot.toml includes)."""
    try:
        from unswbc.project import Project
        project = Project.from_dir(bot_dir)
        project.collect_sources()
        names = project.sources
    except ImportError:
        names = [p.relative_to(bot_dir).as_posix() for p in bot_dir.rglob("*") if p.is_file()]
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in names:
            archive.write(bot_dir / name, name)
    return len(buffer.getvalue())


def run_gate(bot_dir: pathlib.Path, actor: Actor, integer: IntegerActor, mask_level: int = 1, games: int = 24,
             sandbox: bool = True, sandbox_turns: int = 3000) -> GateReport:
    from .export_bot import shared_files_match

    bot_dir = pathlib.Path(bot_dir)
    report = GateReport()
    report.shared_core_identical = shared_files_match(bot_dir)
    sequences = record(integer, mask_level, games)
    report.turns = sum(len(s.blocks) for s in sequences)
    report.float_agreement = float_agreement(actor, integer, sequences)
    native_check(bot_dir, sequences, report)
    if sandbox:
        # Every dragon's sequence is checked natively; the sandbox gets an even
        # spread of them up to its turn budget.
        spread = sequences[::max(1, len(sequences) // 200)]
        sandbox_check(bot_dir, spread, report, sandbox_turns)
    report.zip_bytes = zip_size(bot_dir)
    return report
