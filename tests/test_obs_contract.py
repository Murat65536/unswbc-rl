"""Cross-checks that sim/obs.py (used during training) and
export/bot_template/bot_encoding.py (shipped in the submitted bot) encode
the exact same observation for the same game state.

This feeds a synthetic game state through the REAL helper.py (the one
`unswbc init` generates and the judge runs), not a mock, by serializing it
to the actual wire-protocol text (see protocol_fixture.py). If this test
ever fails after editing either encoder, a trained policy will not play
the same way once exported -- fix the mismatch before training again.
"""
import io
import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from sim import obs as sim_obs
from sim.engine import Dragon, Engine
from sim.map import random_symmetric_map

from .protocol_fixture import build_init_text, build_round_text

_BOT_DIR = ROOT / "bots" / "rl_bot"  # created by `unswbc init python bots/rl_bot`
sys.path.insert(0, str(_BOT_DIR))


def _load_real_helper_and_encoder():
    import helper as real_helper  # noqa: the real helper.py from unswbc init
    sys.path.insert(0, str(ROOT / "export" / "bot_template"))
    import bot_encoding
    return real_helper, bot_encoding


def test_training_and_bot_encoders_agree():
    if not (_BOT_DIR / "helper.py").exists():
        import pytest
        pytest.skip(f"run `unswbc init python {_BOT_DIR}` first (needs the real helper.py)")

    real_helper, bot_encoding = _load_real_helper_and_encoder()

    rng = random.Random(123)
    game_map = random_symmetric_map(20, 20, rng, kelp_prob=0.1, portal_pairs=2)
    a = Dragon(id=0, team="A", body=[(10, 10), (9, 10), (8, 10)])
    b = Dragon(id=1, team="B", body=[(5, 5), (5, 6), (5, 7)])
    engine = Engine(game_map, [a, b], random.Random(7), max_rounds=300)
    engine.round = 17
    # Sprinkle a few pearls into the vision window for coverage.
    for (x, y) in [(9, 9), (11, 10), (10, 12)]:
        engine.pearl_present[y][x] = True

    expected = sim_obs.encode_observation(engine, 0)

    protocol_text = build_init_text(a, game_map) + build_round_text(engine, 0)
    old_stdin = sys.stdin
    sys.stdin = io.StringIO(protocol_text)
    try:
        real_helper.ct = None
        real_helper.game = None
        ct, game = real_helper.init()
        assert real_helper.update(ct, game)
    finally:
        sys.stdin = old_stdin

    actual = bot_encoding.encode_observation(ct, game)

    assert len(expected) == len(actual) == sim_obs.OBS_SIZE
    for i, (e, a_) in enumerate(zip(expected, actual)):
        assert abs(e - a_) < 1e-6, f"feature {i} differs: sim={e} bot={a_}"
