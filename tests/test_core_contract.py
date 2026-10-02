"""The shared core's contract: training and the bot see the same thing.

Training builds a dragon's View straight from the engine (core/state_view.h);
the bot parses it from the protocol text (core/view.h BlockReader). Both feed
the same header-only code for the observation, the masks and the action
decoding (core/features.h), so if the two Views agree, everything agrees.
This checks that they do on every turn of scripted games on the official and
generated maps, that the masks are sound (level 0 and level 1 never hide an
action that could survive; level 2 only narrows the queen's), and pins the
egocentric layout, the queen and threat features, and the action decoding on
hand-built positions.
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


class Brains:
    """Each dragon's Brain (its process's memory), fed every turn, from the
    engine's state as training builds it."""

    def __init__(self):
        self.brains = {}

    def observe(self, match, did):
        return self.brains.setdefault(did, bccore.Brain()).observe_match(match, did)


@pytest.mark.parametrize("label,text,seed,styles", GAMES, ids=[game[0] for game in GAMES])
def test_training_view_equals_bot_view(label, text, seed, styles):
    """Turn by turn, the View built from the state equals the one parsed from
    the blocks; and each dragon's Brain, fed one way or the other, remembers
    the same and computes the same observation and masks."""
    ours, bots = Brains(), {}
    turns = 0

    def check(match, did, init, block):
        nonlocal turns
        difference = match.view_difference(did)
        assert difference is None, f"{label}: dragon {did}: {difference}"
        a = ours.observe(match, did)
        bot = bots.setdefault(did, bccore.Brain())
        b = bot.observe(init, block)
        for x, y in zip(a, b):
            assert np.array_equal(x, y)
        assert ours.brains[did].body == bot.body
        turns += 1

    play_stepwise(text, seed, styles, check)
    assert turns > 0


def test_memory_of_the_body_is_never_wrong():
    """What a dragon remembers of its body is always its real body, head
    first, and soon all of it: a body trails the head's path."""
    brains = Brains()
    stats = {"turns": 0, "whole": 0, "beyond_window": 0}

    def check(match, did, init, block):
        brains.observe(match, did)
        remembered = brains.brains[did].body
        body = [tuple(cell) for cell in match.dragons()[did][4]]
        assert remembered == body[:len(remembered)], (did, remembered, body)
        stats["turns"] += 1
        stats["whole"] += len(remembered) == len(body)
        head = body[0]
        stats["beyond_window"] += any(max(abs(x - head[0]), abs(y - head[1])) > 3 for x, y in remembered)

    for label, text, seed, styles in GAMES:
        play_stepwise(text, seed, styles, check)
    assert stats["whole"] > 0.9 * stats["turns"] and stats["beyond_window"] > 100, stats


def test_masks_never_hide_a_move_that_survives():
    """Every action level 0 masks, and every action level 1 masks while it
    leaves anything, kills the dragon when played (checked on a copy of the
    board). Level 2 only narrows a queen's level-1 choices (its shield), and
    lets it split only with no move left."""
    stats = {"masked0": 0, "masked1": 0, "shielded": 0, "unmasked_fatal": 0, "turns": 0}
    brains = Brains()

    def check(match, did, init, block):
        _, (mask0, mask1, mask2) = brains.observe(match, did)
        assert mask0.any() and mask1.any() and mask2.any()
        assert (mask2 <= mask1).all()
        if did <= 1:
            assert not (mask2[12] and mask2[:12].any()), f"the queen may split only with no move left\n{block}"
        for action in range(bccore.NUM_ACTIONS):
            if not mask0[action]:
                assert match.probe(action), f"level 0 masked a surviving action {action}\n{block}"
                stats["masked0"] += 1
            elif not mask1[action]:
                assert match.probe(action), f"level 1 masked a surviving action {action}\n{block}"
                stats["masked1"] += 1
            elif not mask2[action]:
                stats["shielded"] += 1
            elif match.probe(action):
                stats["unmasked_fatal"] += 1
        stats["turns"] += 1

    for label, text, seed, styles in GAMES:
        brains.brains.clear()
        play_stepwise(text, seed, styles, check)
    assert stats["masked0"] > 100 and stats["masked1"] > 1000 and stats["shielded"] > 100, stats


def test_observation_codes_are_in_range():
    scales = bccore.feature_scales()
    seen_max = np.zeros(bccore.OBS_SIZE, dtype=np.int64)

    brains = Brains()

    def check(match, did, init, block):
        obs, _ = brains.observe(match, did)
        np.maximum(seen_max, obs, out=seen_max)

    for label, text, seed, styles in GAMES[:6]:
        brains.brains.clear()
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
    obs, (mask0, mask1, _) = m.features(0)
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
    assert decode[13] == "SPLIT 2"     # shed
    assert decode[14] == ""            # dissolve: no action at all


def test_level_zero_rules():
    m = solo([(8, 8), (7, 8)])
    _, (mask0, mask1, _) = m.features(0)
    assert mask0[:12].all()
    assert mask0[12] == 0          # length 2 cannot split
    assert not mask1[3:12].any()   # nor pay for a second step: level 1 sees no pearl ahead
    m = solo([(8, 8), (7, 8)])
    m.set_pearl(9, 8)
    _, (_, mask1, _) = m.features(0)
    assert list(mask1[3:6]) == [1, 1, 1]  # unless the first step eats one
    assert not m.probe(3)
    m = solo([(8, 8), (7, 8), (6, 8), (5, 8)])
    _, (mask0, _, _) = m.features(0)
    assert mask0.all()
    m = bccore.Match(make_map(16, 16, [("A", [(8, 8), (7, 8), (6, 8), (5, 8)]), ("B", [(12, 12), (13, 12)]),
                                       ("A", [(2, 2), (2, 3)])], unit_limit=2), 0)
    m.begin()
    _, (mask0, _, _) = m.features(0)
    assert mask0[12] == 0 and mask0[13] == 0  # the unit limit is reached


def test_level_one_sees_through_a_visible_portal():
    # A portal on the head's east side joined to the west side of (11, 6),
    # in sight: stepping east lands on (11, 6), a body segment -> fatal.
    body = [(8, 8), (7, 8), (6, 8)]
    blocker = [(11, 5), (11, 6), (11, 7)]
    text = make_map(16, 16, [("A", body), ("B", blocker)], portals=[(5, "W", 9, 8), (5, "W", 11, 6)])
    m = bccore.Match(text, 0)
    m.begin()
    obs, (mask0, mask1, _) = m.features(0)
    assert mask0[0] == 1 and mask1[0] == 0
    assert scalar(obs, 19) == 0     # the landing tile is known
    assert m.probe(0)


def test_portal_out_of_sight_is_unknown_and_unmasked():
    body = [(8, 8), (7, 8), (6, 8)]
    text = make_map(16, 16, [("A", body), ("B", [(13, 13), (14, 13)])], portals=[(5, "W", 9, 8), (5, "W", 2, 14)])
    m = bccore.Match(text, 0)
    m.begin()
    obs, (_, mask1, _) = m.features(0)
    assert scalar(obs, 19) == 1 and mask1[0] == 1


@pytest.mark.parametrize("pearl", [False, True])
def test_level_one_knows_the_tail_moves_on(pearl):
    # Head (8, 8) facing west, tail (7, 9). Forward then left goes W to (7, 8)
    # then S onto the tail's tile: safe, as the tail moves on at the first
    # step, unless a pearl there makes the dragon grow and the tail stay.
    m = solo([(8, 8), (9, 8), (9, 9), (8, 9), (7, 9)])
    if pearl:
        m.set_pearl(7, 8)
    _, (_, mask1, _) = m.features(0)
    forward_then_left = 3 + 3 * 0 + 2
    assert m.probe(forward_then_left) == pearl
    assert mask1[forward_then_left] == (0 if pearl else 1)
    assert mask1[2] == 0 and m.probe(2)  # a single step left is into the body


def test_reach_sees_a_pocket():
    # Facing east at (8, 8); the tile ahead, (9, 8), is closed by kelp on its
    # north, east and south sides: stepping forward is safe now but a trap.
    m = solo([(8, 8), (7, 8), (6, 8)], kelp=[("N", 9, 8), ("W", 10, 8), ("N", 9, 9)])
    obs, (_, mask1, _) = m.features(0)
    assert mask1[0] == 1                                   # the step itself is not fatal
    assert scalar(obs, 32) == 1 and scalar(obs, 35) == 0   # forward: one tile, going nowhere
    assert scalar(obs, 33) > 20 and scalar(obs, 36) == 1   # right: open water, out of sight
    assert scalar(obs, 34) > 20 and scalar(obs, 37) == 1


def queen_position():
    """A's queen (0, length 4, facing east at (8, 8)) between an enemy head
    right ahead (dragon 3, facing west at (9, 8)) and an ally head on its
    left (dragon 2, facing south at (8, 7)); B's queen (1) far away."""
    m = bccore.Match(make_map(16, 16, [("A", [(8, 8), (7, 8), (6, 8), (5, 8)]), ("B", [(13, 13), (14, 13)]),
                                       ("A", [(8, 7), (8, 6), (8, 5)]), ("B", [(9, 8), (10, 8), (11, 8)])]), 0)
    m.begin()
    return m


def test_head_trades_and_the_queen():
    m = queen_position()
    _, (mask0, mask1, mask2) = m.features(0)
    forward, right, left, split = 0, 1, 2, 12
    assert mask0[forward] and mask0[left] and mask0[split]
    assert mask1[forward] == 1     # onto an enemy head: a trade, the policy's call...
    assert mask1[left] == 0        # ...but never onto an ally's head (two of ours die)
    assert m.probe(left) and m.probe(forward)
    assert mask1[split] == 1 and mask1[right] == 1
    assert mask2[forward] == 0     # the queen trades with nobody
    assert mask2[split] == 0       # and never splits while it has a move
    # Right ends at (8, 9), which dragon 3 could sprint to via (9, 9); right
    # then left at (9, 9), one step from its head. Both are safe for now, but
    # the queen's shield keeps out of reach while it can: two steps right,
    # then forward or right again, end where dragon 3 cannot get.
    right_then_left = 3 + 3 * 1 + 2
    for a in (right, right_then_left):
        assert mask1[a] == 1 and not m.probe(a)
        assert mask2[a] == 0
    assert [a for a in range(12) if mask2[a]] == [3 + 3 * 1 + 0, 3 + 3 * 1 + 1]
    # The ally (2) faces south onto the queen's head: masked at level 1. Its
    # other moves end diagonally from the queen's head, out of its way.
    _, (_, mask1, mask2) = m.features(2)
    assert mask1[forward] == 0 and (mask2 == mask1).all()
    # The enemy (3) faces west onto A's queen's head: a trade it may make.
    _, (_, mask1, mask2) = m.features(3)
    assert mask1[forward] == 1 and mask2[forward] == 1


@pytest.mark.parametrize("enemy_length, threatened", [(4, True), (3, False)])
def test_the_queen_keeps_away_from_enemy_tails(enemy_length, threatened):
    # The queen faces east at (8, 8). An enemy lies along row 9 heading west,
    # its tail at (9, 9): split, its tail becomes a child's head facing west,
    # which moves later in the same round and could step onto (9, 8), where
    # the queen's step forward ends, or (8, 9), where its step right does.
    # Its head is too far to get there; a dragon of length 3 cannot split.
    enemy = [(9 + enemy_length - 1 - i, 9) for i in range(enemy_length)]
    m = bccore.Match(make_map(16, 16, [("A", [(8, 8), (7, 8), (6, 8)]), ("B", [(13, 2), (14, 2)]),
                                       ("B", enemy)]), 0)
    m.begin()
    brain = bccore.Brain()
    obs, (_, mask1, mask2) = brain.observe_match(m, 0)
    threat, _ = brain.threat
    forward, right, left = 0, 1, 2
    assert scalar(obs, 38 + forward) == 0 and scalar(obs, 38 + right) == 0   # no head's reach
    assert threat[forward] == threat[right] == (2 if threatened else 0)
    # The observation says how close: 5 - steps, the tail's child one step
    # from (9, 8), or the head three.
    assert scalar(obs, 56 + forward) == (4 if threatened else 2)
    for a in (forward, right, left):
        assert mask1[a] == 1 and not m.probe(a)
    # With the tail a threat, the queen's shield keeps out of the child's
    # reach: forward and right end one step from it, and every other move
    # but these three within two: left to (8, 7), and two steps left, to
    # (8, 6) or (7, 7). Without, all three single steps are fine.
    if threatened:
        assert [a for a in range(12) if mask2[a]] == [left, 3 + 3 * 2 + 0, 3 + 3 * 2 + 2]
    else:
        assert mask2[forward] == mask2[right] == mask2[left] == 1


def test_queen_planes():
    m = queen_position()
    own_queen, enemy_queen = 22, 23
    obs, _ = m.features(0)          # the queen sees itself as its team's queen
    assert plane(obs, own_queen).sum() == 4 and plane(obs, own_queen)[3, 3] == 1
    assert plane(obs, enemy_queen).sum() == 0
    obs, _ = m.features(2)          # its ally sees the queen, not itself
    assert plane(obs, own_queen).sum() == 4 and plane(obs, own_queen)[3, 3] == 0
    obs, _ = m.features(3)          # the enemy sees A's queen (3 segments in sight) as the enemy queen
    assert plane(obs, enemy_queen).sum() == 3 and plane(obs, own_queen).sum() == 0


def test_enemy_reach():
    m = queen_position()
    obs, _ = m.features(0)
    head_now, forward, right, left = 50, 38, 39, 40
    # Dragon 3's head is one step from the queen's head: reachable in one.
    assert scalar(obs, head_now) == 2
    # Forward and left end on heads (trades): no landing tile.
    assert scalar(obs, forward) == 0 and scalar(obs, left) == 0
    # Right (south, to (8, 9)): dragon 3 could get there in two, via (9, 9).
    assert scalar(obs, right) == 1
    # Right then forward (to (8, 10)): out of its reach.
    assert scalar(obs, 38 + 3 + 3 * 1 + 0) == 0
    # The ally's own head, (8, 7), is two steps from dragon 3, via (9, 7).
    obs, _ = m.features(2)
    assert scalar(obs, head_now) == 1


def test_the_queen_keeps_out_of_pockets():
    # Facing east at (8, 8); the tile ahead is closed on its other three
    # sides by kelp: a pocket of one tile, smaller than the dragon.
    kelp = [("N", 9, 8), ("W", 10, 8), ("N", 9, 9)]
    for queen in (True, False):
        body = [(8, 8), (7, 8), (6, 8)]
        dragons = [("A", body), ("B", [(12, 12), (13, 12)])] if queen else \
            [("A", [(2, 2), (2, 3)]), ("B", [(12, 12), (13, 12)]), ("A", body)]
        m = bccore.Match(make_map(16, 16, dragons, kelp=kelp), 0)
        m.begin()
        did = 0 if queen else 2
        _, (_, mask1, mask2) = m.features(did)
        assert mask1[0] == 1 and not m.probe(0)       # stepping in is not fatal yet
        assert mask2[0] == (0 if queen else 1)        # but the queen's shield keeps it out
        assert mask2[1] == 1 and mask2[2] == 1


def test_a_cornered_dragon_dies_alone():
    # The queen (0) at (8, 8) facing north, kelp ahead and on its right, an
    # ally's head (2) on its left: every move is fatal, but it never takes
    # the ally with it. (With nothing safe, level 2 falls back as level 1.)
    m = bccore.Match(make_map(16, 16, [("A", [(8, 8), (8, 9), (8, 10)]), ("B", [(12, 12), (13, 12)]),
                                       ("A", [(7, 8), (6, 8), (5, 8)])], kelp=[("N", 8, 8), ("W", 9, 8)]), 0)
    m.begin()
    _, (mask0, mask1, mask2) = m.features(0)
    assert all(m.probe(a) for a in range(12))
    assert list(mask0[:12]) == [1] * 12
    left_moves = [2, 9, 10, 11]                        # left, and two-step moves through it
    assert [a for a in range(12) if not mask1[a]] == left_moves
    assert (mask2 == mask1).all()


def test_survival_sees_a_dead_end():
    # The queen faces east at (8, 8), length 3; ahead is a corridor one tile
    # wide, (9, 8) to (11, 8), closed at its end: three moves in, then nothing.
    kelp = [("N", x, 8) for x in (9, 10, 11)] + [("N", x, 9) for x in (9, 10, 11)] + [("W", 12, 8)]
    m = solo([(8, 8), (7, 8), (6, 8)], kelp=kelp)
    brain = bccore.Brain()
    _, (_, mask1, mask2) = brain.observe_match(m, 0)
    survive, horizon = brain.survival
    assert horizon == 8
    assert survive[0] == 3                     # forward: three moves, and stuck
    assert survive[1] == horizon and survive[2] == horizon
    assert mask1[0] == 1 and not m.probe(0)    # not fatal yet
    assert mask2[0] == 0                       # but the queen's shield keeps out of it
    assert mask2[1] == 1 and mask2[2] == 1


@pytest.mark.parametrize("length,lasts", [(3, True), (4, False)])
def test_survival_knows_the_tail_moves_on(length, lasts):
    # A 2x2 room walled by kelp. Three long, the dragon can circle it for
    # ever, stepping where its tail has just left; four long, it fills it.
    kelp = [("N", 8, 8), ("N", 9, 8), ("N", 8, 10), ("N", 9, 10),
            ("W", 8, 8), ("W", 8, 9), ("W", 10, 8), ("W", 10, 9)]
    body = [(8, 8), (8, 9), (9, 9), (9, 8)][:length]
    m = solo(body, kelp=kelp)
    brain = bccore.Brain()
    _, (mask0, mask1, _) = brain.observe_match(m, 0)
    survive, horizon = brain.survival
    if lasts:
        assert survive[1] == horizon           # right, into the free corner: for ever
        for _ in range(12):                    # and so it does, in the engine
            assert not m.probe(1)
            m.reply(brain.decode(1) + "\nENDTURN\n")
            m.reply("MOVE N\nENDTURN\n")     # the far dragon (B)
            brain.observe_match(m, 0)
            assert brain.survival[0][1] == horizon
    else:
        assert all(m.probe(a) for a in range(12))
        assert max(survive[:3]) == 0


def test_allies_keep_out_of_the_queens_way():
    # The queen (0) faces east at (8, 8); an ally (2) faces south at (8, 6).
    # Its step forward, to (8, 7), would stand beside the queen's head.
    m = bccore.Match(make_map(16, 16, [("A", [(8, 8), (7, 8), (6, 8)]), ("B", [(13, 13), (14, 13)]),
                                       ("A", [(8, 6), (8, 5), (8, 4)])]), 0)
    m.begin()
    _, (_, mask1, mask2) = m.features(2)
    forward, right, left = 0, 1, 2
    assert mask1[forward] == 1 and mask2[forward] == 0
    assert mask2[right] == 1 and mask2[left] == 1


SPLIT, SHED, DISSOLVE = 12, 13, 14


@pytest.mark.parametrize("length,sheds", [(5, False), (6, True)])
def test_the_queen_sheds_to_grow_its_team(length, sheds):
    # A queen alone in open water: from length 6 it may shed two segments
    # (SPLIT 2) beside its moves; the half split stays a last resort.
    body = [(8 - i, 8) for i in range(length)]
    m = solo(body)
    _, (mask0, mask1, mask2) = m.features(0)
    assert mask0[SHED] == 1 and mask1[SHED] == 1
    assert mask2[SHED] == (1 if sheds else 0)
    assert mask2[SPLIT] == 0 and mask2[:3].any()
    assert not m.probe(SHED)
    m.reply(bccore.decode_action(m.init_block(0), m.round_block(0), SHED) + "\nENDTURN\n")
    assert len(m.dragons()[0][4]) == length - 2 and len(m.dragons()[2][4]) == 2


def test_dissolving_only_feeds_the_queen():
    # Dragon 2 (A, length 3), its head two steps from its queen's, may dissolve;
    # far from it, it may not. The queen never does, and it is always death.
    near = bccore.Match(make_map(16, 16, [("A", [(8, 8), (7, 8), (6, 8)]), ("B", [(13, 13), (14, 13)]),
                                          ("A", [(9, 7), (9, 6), (9, 5)])]), 0)
    far = bccore.Match(make_map(16, 16, [("A", [(8, 8), (7, 8), (6, 8)]), ("B", [(13, 13), (14, 13)]),
                                         ("A", [(2, 2), (2, 3), (2, 4)])]), 0)
    for m in (near, far):
        m.begin()
    obs, (mask0, mask1, mask2) = near.features(2)
    assert mask0[DISSOLVE] == 1 and mask1[DISSOLVE] == 1 and mask2[DISSOLVE] == 1
    assert scalar(obs, 55) == 1
    obs, (_, mask1, _) = far.features(2)
    assert mask1[DISSOLVE] == 0 and scalar(obs, 55) == 0
    _, (_, mask1, mask2) = near.features(0)
    assert mask1[DISSOLVE] == 0 and mask2[DISSOLVE] == 0
    assert near.probe(DISSOLVE)
    # Dissolving leaves its body as pearls, every second segment from the head.
    near.reply("MOVE E\nENDTURN\n")       # the queen
    near.reply("MOVE N\nENDTURN\n")       # B
    near.reply("ENDTURN\n")                # dragon 2: no action
    assert not near.dragons()[2][2]
    pearls, _ = near.tiles()
    assert pearls[7][9] and not pearls[6][9] and pearls[5][9]
