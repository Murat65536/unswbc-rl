"""A from-scratch, single-game implementation of the UNSW Battlecode 2026
("Dragons") rules, built for fast RL rollouts.

This is deliberately a *re-implementation*, not a wrapper around the real
`unswbc` engine (which talks to bot subprocesses over a text protocol --
far too slow to drive millions of RL steps). It is written directly from
docs/rules/*.md. See README.md's "Known limitations" for what is
intentionally out of scope for v1 (splitting, sonar, official .map file
parsing).

Coordinate system: (0, 0) is top-left, x grows right, y grows down, matching
the real game. Directions: N=(0,-1), E=(1,0), S=(0,1), W=(-1,0).
"""
from __future__ import annotations

import random
from dataclasses import dataclass, field

from . import constants as C
from .map import GameMap


def direction_between(a, b):
    """Direction you'd travel to get from tile `a` to adjacent tile `b`."""
    dx, dy = b[0] - a[0], b[1] - a[1]
    for d, (ox, oy) in C.OFFSET.items():
        if (ox, oy) == (dx, dy):
            return d
    return "E"


@dataclass
class Dragon:
    id: int
    team: str  # 'A' or 'B'
    body: list  # [(x, y), ...] head-first
    alive: bool = True
    facing: str = "E"
    death_reason: str | None = None

    def __post_init__(self):
        if len(self.body) > 1:
            self.facing = direction_between(self.body[1], self.body[0])

    @property
    def length(self):
        return len(self.body)

    @property
    def head(self):
        return self.body[0]


class Engine:
    def __init__(self, game_map: GameMap, dragons, rng: random.Random,
                 max_rounds: int = C.MAX_ROUNDS, unit_limit: int = C.DEFAULT_UNIT_LIMIT):
        self.map = game_map
        self.rng = rng
        self.round = 0
        self.max_rounds = max_rounds
        self.unit_limit = unit_limit
        self.dragons: dict[int, Dragon] = {d.id: d for d in dragons}
        self.done = False
        self.winner: str | None = None  # 'A', 'B', or None (draw, only valid once done)
        self.events = []  # (kind, ...) log, used by tests / reward shaping

        # Queen = lowest-id dragon per team, fixed at spawn.
        self.queen_id = {}
        for team in ("A", "B"):
            ids = [d.id for d in dragons if d.team == team]
            if ids:
                self.queen_id[team] = min(ids)

        w, h = game_map.width, game_map.height
        self.pearl_present = [[False] * w for _ in range(h)]
        self.pearl_countdown = [[-1] * w for _ in range(h)]

        self.occupancy: dict[tuple, int] = {}
        self.heads: dict[tuple, int] = {}
        for d in dragons:
            for pos in d.body:
                self.occupancy[pos] = d.id
            self.heads[d.head] = d.id

        self._init_pearl_countdowns()

    # ------------------------------------------------------------------
    # Setup
    # ------------------------------------------------------------------
    def _mirror(self, x, y):
        w, h = self.map.width, self.map.height
        return w - 1 - x, h - 1 - y

    def _init_pearl_countdowns(self):
        w, h = self.map.width, self.map.height
        seen = set()
        for y in range(h):
            for x in range(w):
                if (x, y) in seen:
                    continue
                mx, my = self._mirror(x, y)
                seen.add((x, y))
                seen.add((mx, my))
                maxg = self.map.pearl_max[y][x]
                if maxg <= 0:
                    self.pearl_countdown[y][x] = -1
                    self.pearl_countdown[my][mx] = -1
                    continue
                ming = self.map.pearl_min[y][x]
                val = self.rng.randint(ming, maxg)
                self.pearl_countdown[y][x] = val
                self.pearl_countdown[my][mx] = val

    # ------------------------------------------------------------------
    # Per-round stepping
    # ------------------------------------------------------------------
    def step(self, actions: dict):
        """Advances exactly one round.

        `actions` maps a living dragon's id to either:
          - a direction string, e.g. "N" (single-step move)
          - a list of direction strings, e.g. ["N", "N", "E"] (a sprint)
          - None (no action -> suicide, per the real engine's default)

        Dragons with no entry in `actions` are also treated as having taken
        no action (suicide), matching "each dragon must output at least one
        action per turn".
        """
        if self.done:
            raise RuntimeError("step() called after the game already ended")

        self.round += 1
        self._tick_pearls()

        order = sorted(did for did, d in self.dragons.items() if d.alive)
        for did in order:
            dragon = self.dragons[did]
            if not dragon.alive:
                continue  # died earlier this round (e.g. a head-to-head)
            action = actions.get(did)
            if action is None:
                self._kill(dragon, C.DEATH_NO_VALID_ACTION)
                continue
            directions = [action] if isinstance(action, str) else list(action)
            if not directions:
                self._kill(dragon, C.DEATH_NO_VALID_ACTION)
                continue
            self._apply_move(dragon, directions)

        self._check_end()

    def _tick_pearls(self):
        w, h = self.map.width, self.map.height
        seen = set()
        for y in range(h):
            for x in range(w):
                if (x, y) in seen:
                    continue
                mx, my = self._mirror(x, y)
                seen.add((x, y))
                seen.add((mx, my))
                if self.pearl_countdown[y][x] < 0:
                    continue
                newcd = self.pearl_countdown[y][x] - 1
                if newcd <= 0:
                    ming, maxg = self.map.pearl_min[y][x], self.map.pearl_max[y][x]
                    drawn = self.rng.randint(ming, maxg)
                    for (xx, yy) in {(x, y), (mx, my)}:
                        if not self.pearl_present[yy][xx] and (xx, yy) not in self.occupancy:
                            self.pearl_present[yy][xx] = True
                        self.pearl_countdown[yy][xx] = drawn
                else:
                    for (xx, yy) in {(x, y), (mx, my)}:
                        self.pearl_countdown[yy][xx] = newcd

    def _apply_move(self, dragon: Dragon, directions: list):
        l0 = dragon.length
        free_steps = -(-l0 // 4)  # ceil(L / 4)
        for i, direction in enumerate(directions):
            if not dragon.alive:
                break
            paid_step = i >= free_steps
            if paid_step and dragon.length <= C.MIN_LENGTH:
                self._kill(dragon, C.DEATH_NO_VALID_ACTION)
                break
            if not self._attempt_single_step(dragon, direction):
                break
            if paid_step and dragon.alive and dragon.length > 1:
                tail = dragon.body.pop()
                if self.occupancy.get(tail) == dragon.id:
                    del self.occupancy[tail]

    def _attempt_single_step(self, dragon: Dragon, direction: str) -> bool:
        """Performs one tile-step of `dragon`'s move. Returns False (and kills
        the dragon, possibly another too) if the step was fatal."""
        x, y = dragon.head
        dragon.facing = direction
        etype, _ = self.map.edge(x, y, direction)
        if etype == C.EDGE_KELP:
            self._kill(dragon, C.DEATH_WALL)
            return False
        if etype == C.EDGE_PORTAL:
            nx, ny = self.map.portal_destination(x, y, direction)
        else:
            dx, dy = C.OFFSET[direction]
            nx, ny = self.map.wrap(x + dx, y + dy)
        dest = (nx, ny)

        if dest in self.heads:
            other = self.dragons[self.heads[dest]]
            self._kill(other, C.DEATH_HEAD_TO_HEAD)
            self._kill(dragon, C.DEATH_HEAD_TO_HEAD)
            return False
        if dest in self.occupancy:
            owner_id = self.occupancy[dest]
            reason = C.DEATH_SELF if owner_id == dragon.id else C.DEATH_OTHER_BODY
            self._kill(dragon, reason)
            return False

        ate_pearl = self.pearl_present[ny][nx]
        old_head = dragon.head
        del self.heads[old_head]
        dragon.body.insert(0, dest)
        self.heads[dest] = dragon.id
        self.occupancy[dest] = dragon.id
        if ate_pearl:
            self.pearl_present[ny][nx] = False
            self.events.append(("eat", dragon.id, dest, self.round))
        else:
            tail = dragon.body.pop()
            if self.occupancy.get(tail) == dragon.id:
                del self.occupancy[tail]
        return True

    def _kill(self, dragon: Dragon, reason: str):
        if not dragon.alive:
            return
        dragon.alive = False
        dragon.death_reason = reason
        for i in range(0, len(dragon.body), 2):
            x, y = dragon.body[i]
            self.pearl_present[y][x] = True
        for pos in dragon.body:
            if self.occupancy.get(pos) == dragon.id:
                del self.occupancy[pos]
        if self.heads.get(dragon.head) == dragon.id:
            del self.heads[dragon.head]
        self.events.append(("death", dragon.id, reason, self.round))

    def _check_end(self):
        alive_a = any(d.alive for d in self.dragons.values() if d.team == "A")
        alive_b = any(d.alive for d in self.dragons.values() if d.team == "B")
        if not alive_a and not alive_b:
            self.done, self.winner = True, None
        elif not alive_a:
            self.done, self.winner = True, "B"
        elif not alive_b:
            self.done, self.winner = True, "A"
        elif self.round >= self.max_rounds:
            self.done, self.winner = True, self._tiebreak()

    def _tiebreak(self):
        def queen_length(team):
            d = self.dragons.get(self.queen_id.get(team, -1))
            return d.length if d and d.alive else 0

        def longest_alive(team):
            lens = [d.length for d in self.dragons.values() if d.team == team and d.alive]
            return max(lens) if lens else 0

        def total_length(team):
            return sum(d.length for d in self.dragons.values() if d.team == team and d.alive)

        for metric in (queen_length, longest_alive, total_length):
            a, b = metric("A"), metric("B")
            if a != b:
                return "A" if a > b else "B"
        return None

    # ------------------------------------------------------------------
    # Observation helpers
    # ------------------------------------------------------------------
    def safe_moves(self, dragon_id: int):
        """Directions that will not immediately kill `dragon_id` by running
        into kelp or a non-head body segment (its own or anyone else's).
        Moving onto an enemy/ally head (a "trade") is left unmasked -- it is
        sometimes the right play, so the policy must learn it, not have it
        hidden."""
        dragon = self.dragons[dragon_id]
        x, y = dragon.head
        safe = []
        for d in C.DIRECTIONS:
            etype, _ = self.map.edge(x, y, d)
            if etype == C.EDGE_KELP:
                continue
            if etype == C.EDGE_PORTAL:
                nx, ny = self.map.portal_destination(x, y, d)
            else:
                dx, dy = C.OFFSET[d]
                nx, ny = self.map.wrap(x + dx, y + dy)
            dest = (nx, ny)
            if dest in self.occupancy and dest not in self.heads:
                continue
            safe.append(d)
        return safe

    def _segment_dirs(self, dragon: Dragon):
        """pos -> facing, for every segment of `dragon` (head: its heading;
        body: the direction towards the head, per the spec)."""
        dirs = {dragon.body[0]: dragon.facing}
        for i in range(1, len(dragon.body)):
            dirs[dragon.body[i]] = direction_between(dragon.body[i], dragon.body[i - 1])
        return dirs

    def vision_window(self, dragon_id: int):
        """The 7x7 egocentric window around `dragon_id`'s head, matching the
        real wire protocol's layout: row 0 is 3 tiles north, column 0 is 3
        tiles west, coordinates already wrapped. Returns a list of 49 dicts,
        row-major."""
        dragon = self.dragons[dragon_id]
        hx, hy = dragon.head
        w, h = self.map.width, self.map.height
        r = C.VISION_RADIUS

        seg_dirs_by_dragon = {}
        tiles = []
        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                x, y = (hx + dx) % w, (hy + dy) % h
                part = None
                if (x, y) in self.occupancy:
                    did = self.occupancy[(x, y)]
                    d = self.dragons[did]
                    if did not in seg_dirs_by_dragon:
                        seg_dirs_by_dragon[did] = self._segment_dirs(d)
                    part = {
                        "team": d.team,
                        "id": did,
                        "is_head": (x, y) == d.head,
                        "dir": seg_dirs_by_dragon[did][(x, y)],
                    }
                tiles.append({
                    "pos": (x, y),
                    "has_pearl": self.pearl_present[y][x],
                    "pearl_time": self.pearl_countdown[y][x],
                    "edges": {dirn: self.map.edge(x, y, dirn) for dirn in C.DIRECTIONS},
                    "dragon": part,
                })
        return tiles

    def team_unit_count(self, team: str) -> int:
        return sum(1 for d in self.dragons.values() if d.team == team and d.alive)
