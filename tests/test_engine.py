import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from sim import constants as C
from sim.engine import Dragon, Engine
from sim.map import GameMap


def empty_map(width=10, height=10):
    kelp_h = [[False] * width for _ in range(height)]
    kelp_v = [[False] * width for _ in range(height)]
    portal_h = [[-1] * width for _ in range(height)]
    portal_v = [[-1] * width for _ in range(height)]
    pearl_min = [[1] * width for _ in range(height)]
    pearl_max = [[0] * width for _ in range(height)]  # no spawns by default
    return GameMap(width, height, kelp_h, kelp_v, portal_h, portal_v, pearl_min, pearl_max, {})


def make_engine(game_map, dragons, max_rounds=500, seed=0):
    return Engine(game_map, dragons, random.Random(seed), max_rounds=max_rounds)


def test_basic_move_east():
    m = empty_map()
    d = Dragon(id=0, team="A", body=[(5, 5), (4, 5), (3, 5)])
    e = make_engine(m, [d])
    e.step({0: "E"})
    assert d.alive
    assert d.body == [(6, 5), (5, 5), (4, 5)]
    assert d.facing == "E"


def test_wraparound():
    m = empty_map(width=10, height=10)
    d = Dragon(id=0, team="A", body=[(9, 5), (8, 5), (7, 5)])
    e = make_engine(m, [d])
    e.step({0: "E"})
    assert d.alive
    assert d.head == (0, 5)


def test_eat_pearl_grows():
    m = empty_map()
    d = Dragon(id=0, team="A", body=[(5, 5), (4, 5), (3, 5)])
    e = make_engine(m, [d])
    e.pearl_present[5][6] = True
    e.step({0: "E"})
    assert d.alive
    assert d.length == 4
    assert d.body == [(6, 5), (5, 5), (4, 5), (3, 5)]
    assert not e.pearl_present[5][6]


def test_kelp_kills():
    m = empty_map()
    m.kelp_v[5][6] = True  # west edge of tile (6,5) == east edge of tile (5,5)
    d = Dragon(id=0, team="A", body=[(5, 5), (4, 5), (3, 5)])
    e = make_engine(m, [d])
    e.step({0: "E"})
    assert not d.alive
    assert d.death_reason == C.DEATH_WALL
    # Dies before moving: length 3 -> ceil(3/2) = 2 pearls dropped.
    dropped = sum(row.count(True) for row in e.pearl_present)
    assert dropped == 2


def test_self_collision_into_tail():
    # A 4-long dragon coiled in a 2x2 loop so moving "north" runs straight
    # into its own tail tile, which must be fatal even though the tail
    # would vacate (it hasn't moved yet at the time of the check).
    m = empty_map()
    body = [(5, 5), (6, 5), (6, 4), (5, 4)]
    d = Dragon(id=0, team="A", body=body)
    e = make_engine(m, [d])
    e.step({0: "N"})
    assert not d.alive
    assert d.death_reason == C.DEATH_SELF


def test_head_to_head_kills_both():
    m = empty_map()
    a = Dragon(id=0, team="A", body=[(4, 5), (3, 5), (2, 5)])
    b = Dragon(id=1, team="B", body=[(6, 5), (7, 5), (8, 5)])
    e = make_engine(m, [a, b])
    e.step({0: "E", 1: "W"})
    assert not a.alive and not b.alive
    assert a.death_reason == C.DEATH_HEAD_TO_HEAD
    assert b.death_reason == C.DEATH_HEAD_TO_HEAD


def test_no_action_is_suicide():
    m = empty_map()
    d = Dragon(id=0, team="A", body=[(5, 5), (4, 5), (3, 5)])
    e = make_engine(m, [d])
    e.step({})
    assert not d.alive
    assert d.death_reason == C.DEATH_NO_VALID_ACTION


def test_team_elimination_ends_game():
    m = empty_map()
    a = Dragon(id=0, team="A", body=[(5, 5), (4, 5), (3, 5)])
    b = Dragon(id=1, team="B", body=[(8, 8), (7, 8), (6, 8)])
    e = make_engine(m, [a, b])
    e.step({0: None, 1: "N"})
    assert e.done
    assert e.winner == "B"


def test_tiebreak_by_queen_length():
    m = empty_map()
    a = Dragon(id=0, team="A", body=[(1, 1), (0, 1)])
    b = Dragon(id=1, team="B", body=[(5, 5), (4, 5), (3, 5)])
    e = make_engine(m, [a, b], max_rounds=1)
    e.step({0: "N", 1: "N"})
    assert e.done
    assert e.winner == "B"  # longer queen


def test_safe_moves_excludes_kelp_and_bodies():
    m = empty_map()
    m.kelp_h[5][5] = True  # north edge of (5,5): kelp_h[y][x] for tile (x,y)
    a = Dragon(id=0, team="A", body=[(5, 5), (4, 5), (3, 5)])
    b = Dragon(id=1, team="B", body=[(6, 5), (6, 4), (6, 3)])
    e = make_engine(m, [a, b])
    safe = e.safe_moves(0)
    assert "N" not in safe  # kelp
    assert "W" not in safe  # own body
    assert "E" in safe  # enemy head is a legal (if risky) trade
