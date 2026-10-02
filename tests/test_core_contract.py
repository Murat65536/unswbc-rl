"""The shared core's contract: training and the bot see the same thing.

Training builds a dragon's View straight from the engine (core/state_view.h);
the bot parses it from the protocol text (core/view.h BlockReader). Both feed
the same header-only code for the observation, the masks and the action
decoding (core/features.h), so if the two Views agree, everything agrees.
This checks that they do on every turn of scripted games on the official and
generated maps, that the masks are sound (level 0 and level 1 never hide an
action that could survive), and pins the egocentric layout and the action
decoding on hand-built positions.
"""
import pathlib
import random
import sys

import numpy as np
import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

bccore = pytest.importorskip("bccore", reason="build the C++ core first (see README)")

import lockstep  # noqa: E402
from test_rules import make_map  # noqa: E402

TILES = 49
PLANES = bccore.NUM_PLANES
SCALARS_AT = PLANES * TILES


def official_maps():
    try:
        import unswbc
    except ImportError:
        return []
    folder = pathlib.Path(unswbc.__file__).resolve().parent / "templates" / "maps"
    return sorted(folder.glob("*.map"))


def play_stepwise(text, seed, styles, on_turn):
    """Plays a game a turn at a time with scripted players, calling
    on_turn(match, id, init_block, round_block) before every turn."""
    match = bccore.Match(text, seed)
    players = lockstep.Players(seed, styles)
    match.begin()
    while (did := match.current()) is not None:
        if did not in players.players:
            players.spawn(did, match.init_block(did))
        block = match.round_block(did)
        on_turn(match, did, match.init_block(did), block)
        match.reply(players.reply(did, block))
    return match


def games():
    for path in official_maps():
        yield path.name, path.read_text(), 7, ("mixed", "careful")
    rng = random.Random(11)
    for i in range(9):
        symmetry = ["x", "y", "xy"][i % 3]
        text = bccore.generate_map(rng.randint(10, 30), rng.randint(10, 30), symmetry, seed=i,
                                   dragons_per_team=rng.randint(1, 4), portal_pairs=rng.randint(0, 8),
                                   kelp=rng.choice([0.0, 0.1, 0.25]), unit_limit=rng.choice([64, 4]))
        yield f"generated {symmetry} #{i}", text, i, [("chaotic", "splitter"), ("survivor", "sprinter"),
                                                       ("mixed", "careful")][i % 3]


GAMES = list(games())


@pytest.mark.parametrize("label,text,seed,styles", GAMES, ids=[game[0] for game in GAMES])
def test_training_view_equals_bot_view(label, text, seed, styles):
    turns = 0

    def check(match, did, init, block):
        nonlocal turns
        difference = match.view_difference(did)
        assert difference is None, f"{label}: dragon {did}: {difference}"
        ours = match.features(did)
        bots = bccore.features_from_blocks(init, block)
        for a, b in zip(ours, bots):
            assert np.array_equal(a, b)
        turns += 1

    play_stepwise(text, seed, styles, check)
    assert turns > 0


def test_masks_never_hide_a_move_that_survives():
    """Every action level 0 masks, and every action level 1 masks, kills the
    dragon when played (checked on a copy of the board)."""
    stats = {"masked0": 0, "masked1": 0, "unmasked_fatal": 0, "turns": 0}

    def check(match, did, init, block):
        _, mask0, mask1 = match.features(did)
        for action in range(bccore.NUM_ACTIONS):
            if not mask0[action]:
                assert match.probe(action), f"level 0 masked a surviving action {action}\n{block}"
                stats["masked0"] += 1
            elif not mask1[action] and mask1.any():
                assert match.probe(action), f"level 1 masked a surviving action {action}\n{block}"
                stats["masked1"] += 1
            elif match.probe(action):
                stats["unmasked_fatal"] += 1
        stats["turns"] += 1

    for label, text, seed, styles in GAMES:
        play_stepwise(text, seed, styles, check)
    assert stats["masked0"] > 100 and stats["masked1"] > 1000, stats


def test_observation_codes_are_in_range():
    scales = bccore.feature_scales()
    seen_max = np.zeros(bccore.OBS_SIZE, dtype=np.int64)

    def check(match, did, init, block):
        obs, _, _ = match.features(did)
        np.maximum(seen_max, obs, out=seen_max)

    for label, text, seed, styles in GAMES[:6]:
        play_stepwise(text, seed, styles, check)
    assert (seen_max * scales <= 1.0 + 1e-6).all()
    assert seen_max[:SCALARS_AT].max() <= 32


# --- hand-built positions ------------------------------------------------------

def plane(obs, index):
    return obs[index * TILES:(index + 1) * TILES].reshape(7, 7)


def scalar(obs, index):
    return obs[SCALARS_AT + index]


def solo(body, **kw):
    """A match with dragon 0 (team A) at `body` and a B dragon parked far away."""
    m = bccore.Match(make_map(16, 16, [("A", body), ("B", [(12, 12), (13, 12)])], **kw), 0)
    m.begin()
    return m


@pytest.mark.parametrize("facing,body", [
    ("N", [(8, 8), (8, 9), (8, 10)]),
    ("E", [(8, 8), (7, 8), (6, 8)]),
    ("S", [(8, 8), (8, 7), (8, 6)]),
    ("W", [(8, 8), (9, 8), (10, 8)]),
])
def test_window_is_egocentric(facing, body):
    step = {"N": (0, -1), "E": (1, 0), "S": (0, 1), "W": (-1, 0)}
    right = {"N": "E", "E": "S", "S": "W", "W": "N"}
    fx, fy = step[facing]
    rx, ry = step[right[facing]]
    # A pearl two ahead and one to the right; kelp on the head's forward side.
    px, py = 8 + 2 * fx + rx, 8 + 2 * fy + ry
    side, kx, ky = {"N": ("N", 8, 8), "S": ("N", 8, 9), "W": ("W", 8, 8), "E": ("W", 9, 8)}[facing]
    m = solo(body, kelp=[(side, kx, ky)])
    m.set_pearl(px, py)
    obs, mask0, mask1 = m.features(0)
    pearls = plane(obs, 0)
    assert pearls[1, 4] == 1 and pearls.sum() == 1          # row 1 = two ahead, col 4 = one right
    assert plane(obs, 12)[3, 3] == 1                          # own head at the centre
    assert plane(obs, 13)[4, 3] == 1 and plane(obs, 13)[5, 3] == 1  # body behind
    assert plane(obs, 4)[3, 3] == 1                           # kelp ahead of the head
    assert plane(obs, 18)[3, 3] == 1                          # the head faces forward
    assert plane(obs, 18)[4, 3] == 1                          # the neck points forward, at the head
    assert scalar(obs, 7) == 1 and scalar(obs, 8) == 0 and scalar(obs, 9) == 0
    assert mask0[0] == 1 and mask1[0] == 0                    # level 1 hides the step into kelp


def test_actions_decode_relative_to_the_facing():
    m = solo([(8, 8), (7, 8), (6, 8), (5, 8), (4, 8), (3, 8)])  # facing E, length 6
    init, block = m.init_block(0), m.round_block(0)
    decode = [bccore.decode_action(init, block, a) for a in range(bccore.NUM_ACTIONS)]
    assert decode[:3] == ["MOVE E", "MOVE S", "MOVE N"]
    # Two steps: the second turn is relative to the first step's heading.
    assert decode[3:12] == ["MOVE EE", "MOVE ES", "MOVE EN",
                            "MOVE SS", "MOVE SW", "MOVE SE",
                            "MOVE NN", "MOVE NE", "MOVE NW"]
    assert decode[12] == "SPLIT 3"


def test_level_zero_rules():
    m = solo([(8, 8), (7, 8)])
    _, mask0, mask1 = m.features(0)
    assert mask0[:12].all()
    assert mask0[12] == 0          # length 2 cannot split
    assert not mask1[3:12].any()   # nor pay for a second step: level 1 sees no pearl ahead
    m = solo([(8, 8), (7, 8)])
    m.set_pearl(9, 8)
    _, _, mask1 = m.features(0)
    assert list(mask1[3:6]) == [1, 1, 1]  # unless the first step eats one
    assert not m.probe(3)
    m = solo([(8, 8), (7, 8), (6, 8), (5, 8)])
    _, mask0, _ = m.features(0)
    assert mask0.all()
    m = bccore.Match(make_map(16, 16, [("A", [(8, 8), (7, 8), (6, 8), (5, 8)]), ("B", [(12, 12), (13, 12)]),
                                       ("A", [(2, 2), (2, 3)])], unit_limit=2), 0)
    m.begin()
    _, mask0, _ = m.features(0)
    assert mask0[12] == 0          # the unit limit is reached


def test_level_one_sees_through_a_visible_portal():
    # A portal on the head's east side joined to the west side of (11, 6),
    # in sight: stepping east lands on (11, 6), a body segment -> fatal.
    body = [(8, 8), (7, 8), (6, 8)]
    blocker = [(11, 5), (11, 6), (11, 7)]
    text = make_map(16, 16, [("A", body), ("B", blocker)], portals=[(5, "W", 9, 8), (5, "W", 11, 6)])
    m = bccore.Match(text, 0)
    m.begin()
    obs, mask0, mask1 = m.features(0)
    assert mask0[0] == 1 and mask1[0] == 0
    assert scalar(obs, 19) == 0     # the landing tile is known
    assert m.probe(0)


def test_portal_out_of_sight_is_unknown_and_unmasked():
    body = [(8, 8), (7, 8), (6, 8)]
    text = make_map(16, 16, [("A", body), ("B", [(13, 13), (14, 13)])], portals=[(5, "W", 9, 8), (5, "W", 2, 14)])
    m = bccore.Match(text, 0)
    m.begin()
    obs, _, mask1 = m.features(0)
    assert scalar(obs, 19) == 1 and mask1[0] == 1


@pytest.mark.parametrize("pearl", [False, True])
def test_level_one_knows_the_tail_moves_on(pearl):
    # Head (8, 8) facing west, tail (7, 9). Forward then left goes W to (7, 8)
    # then S onto the tail's tile: safe, as the tail moves on at the first
    # step, unless a pearl there makes the dragon grow and the tail stay.
    m = solo([(8, 8), (9, 8), (9, 9), (8, 9), (7, 9)])
    if pearl:
        m.set_pearl(7, 8)
    _, _, mask1 = m.features(0)
    forward_then_left = 3 + 3 * 0 + 2
    assert m.probe(forward_then_left) == pearl
    assert mask1[forward_then_left] == (0 if pearl else 1)
    assert mask1[2] == 0 and m.probe(2)  # a single step left is into the body


def test_reach_sees_a_pocket():
    # Facing east at (8, 8); the tile ahead, (9, 8), is closed by kelp on its
    # north, east and south sides: stepping forward is safe now but a trap.
    m = solo([(8, 8), (7, 8), (6, 8)], kelp=[("N", 9, 8), ("W", 10, 8), ("N", 9, 9)])
    obs, _, mask1 = m.features(0)
    assert mask1[0] == 1                                   # the step itself is not fatal
    assert scalar(obs, 32) == 1 and scalar(obs, 35) == 0   # forward: one tile, going nowhere
    assert scalar(obs, 33) > 20 and scalar(obs, 36) == 1   # right: open water, out of sight
    assert scalar(obs, 34) > 20 and scalar(obs, 37) == 1
