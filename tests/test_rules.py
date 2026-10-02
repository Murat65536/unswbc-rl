"""Hand-built rule scenarios on the C++ engine (engine/ through bccore),
written from docs/rules/. test_fidelity.py proves the engine matches the
judge; these pin down individual rules so a regression names the rule."""
import pathlib
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

bccore = pytest.importorskip("bccore", reason="build the C++ core first (see README)")


def edge_index(width, side, x, y):
    """Map-file index of the edge on `side` ("N" or "W") of tile (x, y)."""
    row = 2 * y if side == "N" else 2 * y + 1
    return row * (width + 1) + x


def make_map(width, height, dragons, kelp=(), portals=(), beds=(), unit_limit=None):
    """dragons: [(team, [(x, y), ...head first])]; kelp: [(side, x, y)] with
    side "N" or "W"; portals: [(id, side, x, y)]; beds: [(x, y, lo, hi)]."""
    lines = [f"MAP {width} {height}"]
    if unit_limit is not None:
        lines.append(f"UNIT_LIMIT {unit_limit}")
    lines.append(f"TILE_COUNT {len(beds)}")
    lines += [f"TILE {x} {y} {lo} {hi}" for x, y, lo, hi in beds]
    edges = [f"EDGE {edge_index(width, s, x, y)} 1 -1" for s, x, y in kelp]
    edges += [f"EDGE {edge_index(width, s, x, y)} 2 {pid}" for pid, s, x, y in portals]
    lines.append(f"EDGE_COUNT {len(edges)}")
    lines += edges
    lines.append(f"DRAGON_COUNT {len(dragons)}")
    for team, body in dragons:
        lines.append(f"DRAGON {0 if team == 'A' else 1} {len(body)} " + " ".join(f"{x} {y}" for x, y in body))
    return "\n".join(lines) + "\n"


def start(text, seed=0):
    match = bccore.Match(text, seed)
    match.begin()
    return match


def dragon(match, did):
    did_, team, alive, facing, body = match.dragons()[did]
    return {"team": team, "alive": alive, "facing": facing, "body": body}


def far_away(width=12, height=12):
    """A second team parked out of the way, so a game doesn't end early."""
    return ("B", [(width - 2, height - 2), (width - 1, height - 2)])


def test_move_and_facing():
    m = start(make_map(12, 12, [("A", [(5, 5), (4, 5), (3, 5)]), far_away()]))
    assert m.current() == 0
    m.move("E")
    assert dragon(m, 0)["body"] == [(6, 5), (5, 5), (4, 5)]
    assert dragon(m, 0)["facing"] == "E"
    assert m.current() == 1


def test_wraparound():
    m = start(make_map(12, 12, [("A", [(11, 5), (10, 5), (9, 5)]), far_away()]))
    m.move("E")
    assert dragon(m, 0)["body"][0] == (0, 5)


def test_eating_grows_and_tail_stays():
    m = start(make_map(12, 12, [("A", [(5, 5), (4, 5), (3, 5)]), far_away()]))
    m.set_pearl(6, 5)
    m.move("E")
    assert dragon(m, 0)["body"] == [(6, 5), (5, 5), (4, 5), (3, 5)]
    assert not m.tiles()[0][5][6]


def test_kelp_kills_and_drops_every_second_segment():
    m = start(make_map(12, 12, [("A", [(5, 5), (4, 5), (3, 5)]), far_away()], kelp=[("W", 6, 5)]))
    m.move("E")
    assert not dragon(m, 0)["alive"]
    pearls = m.tiles()[0]
    assert pearls[5][5] and not pearls[5][4] and pearls[5][3]  # ceil(3 / 2) = 2, head first


def test_own_tail_is_fatal_before_it_moves():
    body = [(5, 5), (6, 5), (6, 4), (5, 4)]
    m = start(make_map(12, 12, [("A", body), far_away()]))
    m.move("N")
    assert not dragon(m, 0)["alive"]


def test_head_to_head_kills_both_other_first():
    # Dragon 0 steps onto (5, 5); then dragon 1 steps west onto 0's head:
    # the dragon it ran into dies first, then itself.
    a = ("A", [(4, 5), (3, 5), (2, 5)])
    b = ("B", [(6, 5), (7, 5), (8, 5)])
    replies = {0: "MOVE E\nENDTURN\n", 1: "MOVE W\nENDTURN\n"}
    deaths = []
    result = bccore.Match(make_map(12, 12, [a, b]), 0).run(
        lambda did, block: replies[did], death=lambda did, rnd, why: deaths.append((did, why)))
    assert deaths == [(0, "H"), (1, "H")]
    assert result["winner"] is None and result["end_reason"] == 0


def test_no_action_is_suicide():
    m = start(make_map(12, 12, [("A", [(5, 5), (4, 5), (3, 5)]), far_away()]))
    m.reply("LOG nothing to do\nENDTURN\n")
    assert not dragon(m, 0)["alive"]
    # Elimination is checked when the round ends, so B still has its turn.
    assert m.current() == 1
    m.move("N")
    assert m.current() is None
    assert m.result()["winner"] == "B" and m.result()["rounds"] == 0


def test_sprint_first_quarter_is_free():
    # Length 5: ceil(5 / 4) = 2 free steps, so a two-step move keeps length 5
    # (1.0.2 charged the second step).
    body = [(5, 5), (4, 5), (3, 5), (2, 5), (1, 5)]
    m = start(make_map(12, 12, [("A", body), far_away()]))
    m.move("EE")
    assert dragon(m, 0)["body"] == [(7, 5), (6, 5), (5, 5), (4, 5), (3, 5)]


def test_sprint_paid_steps_cost_a_segment_each():
    body = [(5, 5), (4, 5), (3, 5), (2, 5), (1, 5)]
    m = start(make_map(12, 12, [("A", body), far_away()]))
    m.move("EEEE")  # 2 free + 2 paid
    assert dragon(m, 0)["body"] == [(9, 5), (8, 5), (7, 5)]


def test_sprint_it_cannot_pay_for_kills():
    m = start(make_map(12, 12, [("A", [(5, 5), (4, 5), (3, 5)]), far_away()]))
    m.move("EEE")  # 1 free, 1 paid (length 2), then nothing left to pay with
    assert not dragon(m, 0)["alive"]


def test_split_child_is_rear_reversed_and_acts_this_round():
    body = [(5, 5), (4, 5), (3, 5), (2, 5), (1, 5), (1, 6)]
    m = start(make_map(12, 12, [("A", body), far_away()]))
    m.split(3)
    parent, child = dragon(m, 0), dragon(m, 2)
    assert parent["body"] == [(5, 5), (4, 5), (3, 5)]
    assert child["body"] == [(1, 6), (1, 5), (2, 5)]
    assert child["facing"] == "S" and child["team"] == "A"
    assert m.current() == 1
    m.move("N")
    assert m.current() == 2 and m.round == 0  # the child, later in the same round


@pytest.mark.parametrize("n", [1, 5, 0, -1])
def test_illegal_split_kills(n):
    body = [(5, 5), (4, 5), (3, 5), (2, 5), (1, 5), (0, 5)]
    m = start(make_map(12, 12, [("A", body), far_away()]))
    m.split(n)
    assert not dragon(m, 0)["alive"]


def test_split_at_unit_limit_kills():
    body = [(5, 5), (4, 5), (3, 5), (2, 5)]
    m = start(make_map(12, 12, [("A", body), ("A", [(5, 8), (4, 8)]), far_away()], unit_limit=2))
    m.split(2)
    assert not dragon(m, 0)["alive"]


def test_portal_keeps_heading():
    # A portal on the east side of (5, 5) joined to the east side of (8, 2):
    # stepping east through it lands just east of the partner edge.
    m = start(make_map(12, 12, [("A", [(5, 5), (4, 5), (3, 5)]), far_away()],
                       portals=[(7, "W", 6, 5), (7, "W", 9, 2)]))
    m.move("E")
    assert dragon(m, 0)["body"][0] == (9, 2)
    assert dragon(m, 0)["facing"] == "E"


def test_pearls_follow_the_seed():
    text = make_map(12, 12, [("A", [(5, 5), (4, 5)]), far_away()], beds=[(1, 1, 1, 50), (2, 1, 1, 50)])
    first = bccore.Match(text, 1)
    first.begin()
    again = bccore.Match(text, 1)
    again.begin()
    other = bccore.Match(text, 2)
    other.begin()
    assert first.tiles() == again.tiles()
    assert first.tiles()[1][1][1:3] != other.tiles()[1][1][1:3]


def play_to_round_limit(m, moves):
    while m.current() is not None:
        m.move(moves[m.current()])
    return m.result()


def test_round_limit_queen_decides_before_longest():
    # Team A: queen (0) dies at once, dragon 2 is the longest on the board.
    # Team B: queen (1) lives. B wins on the queen, though A's longest is longer.
    north = {1: "N", 2: "N", 3: "N"}
    dragons = [
        ("A", [(1, 1), (0, 1), (11, 1)]),
        ("B", [(5, 5), (5, 6), (6, 6)]),
        ("A", [(9, 9), (9, 10), (9, 11), (9, 0), (9, 1)]),
        ("B", [(2, 8), (2, 9), (2, 10)]),
    ]
    m = start(make_map(12, 12, dragons))
    m.move("W")  # the queen of team A turns back into its own neck
    assert not dragon(m, 0)["alive"]
    # The rest walk north in their own columns forever: on a torus with no
    # pearl beds a dragon shorter than the column never meets anything.
    result = play_to_round_limit(m, north)
    assert result["end_reason"] == 1 and result["rounds"] == 499
    assert result["a_queen"] == 0 and result["b_queen"] == 3
    assert result["a_longest"] == 5 > result["b_longest"]
    assert result["winner"] == "B"
