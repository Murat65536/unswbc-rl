"""Step 1's acceptance test: our engine (engine/, through bccore) plays full
seeded games, pearls included, identically to the judge's real engine.

Both engines get the same map text, the same 64-bit match seed and the same
scripted players (tests/lockstep.py), and every spawn (init block), every turn
(round block and reply), every death and the final result must match. That
covers the 15 official maps, generated maps in all three symmetries (with
kelp, portals and small unit limits), and a hand-built map that forces the
round-limit tiebreaks, over players that sprint (free, paid and unpayable),
split (legal, illegal and at the unit limit), cross portals, hit walls, trade
heads, send sonar in both protocols and write garbage.

The judge's engine is the `unswbc_engine.wasm` that the installed `unswbc`
toolkit ships. It is pinned by hash: a toolkit upgrade that changes it makes
this test fail until it has been rerun against the new engine and the pin
moved (the engine is the same in 1.2.3 and 1.2.4).

FIDELITY_SCALE=<n> multiplies the number of seeds, for a longer run.
"""
import hashlib
import os
import pathlib
import random
import sys
from collections import Counter

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

bccore = pytest.importorskip("bccore", reason="build the C++ core first (see README)")
unswbc = pytest.importorskip("unswbc", reason="pip install -r requirements-dev.txt (the judge's toolkit)")
pytest.importorskip("wasmtime", reason="the judge's toolkit needs wasmtime")

import lockstep  # noqa: E402

JUDGE_ENGINE_SHA256 = "26e68680e45eb0f221db702aead9eefde776c2ad2ba066f4ddf8c12500c6a546"
JUDGE_ENGINE_SHIPPED_BY = ("1.2.3", "1.2.4")

SCALE = int(os.environ.get("FIDELITY_SCALE", "1"))
STYLE_PAIRS = [
    ("survivor", "survivor"),
    ("mixed", "careful"),
    ("chaotic", "sprinter"),
    ("splitter", "survivor"),
    ("careful", "chaotic"),
]
MAPS_DIR = pathlib.Path(unswbc.__file__).resolve().parent / "templates" / "maps"
OFFICIAL_MAPS = sorted(p.name for p in MAPS_DIR.glob("*.map"))


def _seed(*parts) -> int:
    return int.from_bytes(hashlib.sha256(repr(parts).encode()).digest()[:8], "little")


_coverage = Counter()
_games = Counter()


def _check(label: str, map_text: str, seed: int, styles: tuple[str, str]):
    ours = lockstep.play_ours(map_text, seed, styles)
    judge = lockstep.play_judge(map_text, seed, styles)
    difference = lockstep.first_difference(ours, judge)
    assert difference is None, f"{label}, seed {seed:#x}, players {styles}: {difference}"
    _coverage.update(lockstep.coverage(judge))
    _games["games"] += 1
    _games["turns"] += sum(1 for event in judge.stream if event[0] == "turn")


def test_judge_engine_is_the_pinned_one():
    wasm = pathlib.Path(unswbc.__file__).resolve().parent / "unswbc_engine.wasm"
    digest = hashlib.sha256(wasm.read_bytes()).hexdigest()
    assert digest == JUDGE_ENGINE_SHA256, (
        f"unswbc {unswbc.__version__ if hasattr(unswbc, '__version__') else '?'} ships a different engine "
        f"({digest}) from the one engine/ was verified against (the one in unswbc "
        f"{' and '.join(JUDGE_ENGINE_SHIPPED_BY)}). Rerun these tests with FIDELITY_SCALE raised, fix "
        "engine/ until they pass, then update JUDGE_ENGINE_SHA256.")


def test_official_maps_are_all_here():
    assert len(OFFICIAL_MAPS) == 15, OFFICIAL_MAPS


@pytest.mark.parametrize("map_name", OFFICIAL_MAPS)
def test_official_map(map_name):
    text = (MAPS_DIR / map_name).read_text()
    for i in range(2 * SCALE):
        for styles in STYLE_PAIRS:
            _check(map_name, text, _seed(map_name, i, styles), styles)


@pytest.mark.parametrize("symmetry", ["x", "y", "xy"])
def test_generated_maps(symmetry):
    rng = random.Random(_seed("generated", symmetry))
    for i in range(4 * SCALE):
        options = dict(
            dragons_per_team=rng.randint(1, 4),
            start_length=rng.randint(2, 6),
            kelp=rng.choice([0.0, 0.05, 0.15, 0.3]),
            portal_pairs=rng.randint(0, 6),
            pearl_beds=rng.random(),
            gap_min_hi=rng.randint(1, 20),
            gap_spread=rng.randint(0, 200),
            unit_limit=rng.choice([64, 3, 5, 8]),
        )
        width, height = rng.randint(10, 40), rng.randint(10, 40)
        text = bccore.generate_map(width, height, symmetry, seed=rng.getrandbits(64), **options)
        for styles in STYLE_PAIRS:
            _check(f"generated {width}x{height} {symmetry} {options}", text, rng.getrandbits(64), styles)


@pytest.mark.parametrize("styles", [
    ("survivor", "survivor"),   # everything equal: a draw at the round limit
    ("regicide", "loyal"),      # A's queen dead, B's other dragon dead: B wins on the queen alone
    ("survivor", "regicide"),
    ("loyal", "survivor"),      # queens equal: the longest dragon decides
])
def test_round_limit_tiebreaks(styles):
    text = lockstep.ring_map()
    for i in range(SCALE):
        _check("rings", text, _seed("rings", i, styles), styles)


def test_every_rule_situation_was_exercised():
    """The comparisons above prove nothing about situations they never hit, so
    insist that each one came up (counted from the judge's side)."""
    if _games["games"] < 15 * 2 * len(STYLE_PAIRS):
        pytest.skip("run the whole module: this checks what the other tests played")
    required = [
        "death_W", "death_S", "death_O", "death_H", "death_A",
        "free_sprint", "paid_sprint", "unpayable_sprint",
        "legal_split", "illegal_split", "split_at_unit_limit", "child_acts_in_birth_round",
        "portal_step", "pearl_eaten", "no_action",
        "sonar_received", "sonar_64bit_received", "sonar_echoes",
        "ended_by_elimination", "ended_both_eliminated", "ended_at_round_limit",
        "round_limit_queen_decides", "round_limit_later_tiebreak",
    ]
    missing = [name for name in required if _coverage[name] == 0]
    assert not missing, f"never exercised: {missing}; seen {dict(_coverage)}"
    print(f"\n{_games['games']} games, {_games['turns']} turns identical; {dict(sorted(_coverage.items()))}")
