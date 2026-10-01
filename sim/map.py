"""Procedural game maps for training.

Edges are stored without redundancy: ``kelp_h[y][x]`` / ``portal_h[y][x]`` is
the edge between tile (x, y) and tile (x, y-1) (i.e. the tile's north edge,
with wraparound meaning y=0's north edge is also tile y=height-1's south
edge). ``kelp_v[y][x]`` / ``portal_v[y][x]`` is the edge between tile (x, y)
and tile (x-1, y) (the tile's west edge, wrapping the same way).

NOTE: this module only generates training maps. It does not parse the
official ``.map`` file format used by ``unswbc`` -- the exported competition
bot never needs to, since it talks to the real engine over stdio and never
sees a map file directly. See README.md's "Known limitations" section.
"""
from __future__ import annotations

import random
from dataclasses import dataclass, field

from . import constants as C


@dataclass
class GameMap:
    width: int
    height: int
    kelp_h: list  # [height][width] bool
    kelp_v: list  # [height][width] bool
    portal_h: list  # [height][width] int, -1 if not a portal
    portal_v: list  # [height][width] int, -1 if not a portal
    pearl_min: list  # [height][width] int
    pearl_max: list  # [height][width] int (0 => tile never spawns)
    # (axis, y, x) -> (axis, y, x) for the other half of a portal pair.
    portal_partner: dict = field(default_factory=dict)

    def edge(self, x: int, y: int, direction: str):
        """Returns (edge_type, portal_id) for the edge crossed moving `direction`
        out of tile (x, y)."""
        w, h = self.width, self.height
        if direction == "N":
            yy, xx = y % h, x % w
            kelp, portal = self.kelp_h[yy][xx], self.portal_h[yy][xx]
        elif direction == "S":
            yy, xx = (y + 1) % h, x % w
            kelp, portal = self.kelp_h[yy][xx], self.portal_h[yy][xx]
        elif direction == "W":
            yy, xx = y % h, x % w
            kelp, portal = self.kelp_v[yy][xx], self.portal_v[yy][xx]
        elif direction == "E":
            yy, xx = y % h, (x + 1) % w
            kelp, portal = self.kelp_v[yy][xx], self.portal_v[yy][xx]
        else:
            raise ValueError(direction)
        if kelp:
            return C.EDGE_KELP, -1
        if portal >= 0:
            return C.EDGE_PORTAL, portal
        return C.EDGE_EMPTY, -1

    def portal_destination(self, x: int, y: int, direction: str):
        """Tile reached by crossing the portal edge at (x, y, direction).

        Model: the far edge is treated exactly like the near edge would be,
        and the direction of travel is preserved. This keeps portals
        direction-preserving and double-sided, matching the documented
        behaviour, though it is our own convention rather than a byte-exact
        replica of the judge's internal indexing (see README).
        """
        w, h = self.width, self.height
        if direction == "N":
            axis, yy, xx = "h", y % h, x % w
        elif direction == "S":
            axis, yy, xx = "h", (y + 1) % h, x % w
        elif direction == "W":
            axis, yy, xx = "v", y % h, x % w
        else:
            axis, yy, xx = "v", y % h, (x + 1) % w
        axis2, y2, x2 = self.portal_partner[(axis, yy, xx)]
        if direction == "N":
            return x2, (y2 - 1) % h
        if direction == "S":
            return x2, (y2 + 1) % h
        if direction == "W":
            return (x2 - 1) % w, y2
        return (x2 + 1) % w, y2

    def wrap(self, x: int, y: int):
        return x % self.width, y % self.height


def _mirror_xy(w: int, h: int, x: int, y: int):
    """180-degree rotational symmetry."""
    return w - 1 - x, h - 1 - y


def random_symmetric_map(
    width: int,
    height: int,
    rng: random.Random,
    kelp_prob: float = 0.05,
    portal_pairs: int = 0,
    pearl_gap_range=(3, 3),
    pearl_max_range=(15, 45),
    pearl_coverage: float = 0.4,
    spawn_clear_radius: int = 2,
) -> GameMap:
    """Generates a map with 180-degree rotational symmetry.

    This is deliberately simple (no connectivity guarantees) -- it's meant
    to give a self-play policy map *diversity* during training, not to be a
    tournament-legal map. See README's "Known limitations".
    """
    kelp_h = [[False] * width for _ in range(height)]
    kelp_v = [[False] * width for _ in range(height)]
    portal_h = [[-1] * width for _ in range(height)]
    portal_v = [[-1] * width for _ in range(height)]
    pearl_min = [[pearl_gap_range[0]] * width for _ in range(height)]
    pearl_max = [[0] * width for _ in range(height)]

    for y in range(height):
        for x in range(width):
            if rng.random() < pearl_coverage:
                pearl_max[y][x] = rng.randint(*pearl_max_range)

    # Kelp, mirrored under 180-degree rotation.
    seen = set()
    for y in range(height):
        for x in range(width):
            if ("h", y, x) not in seen:
                val = rng.random() < kelp_prob
                kelp_h[y][x] = val
                my, mx = (height - y) % height, (width - 1 - x) % width
                kelp_h[my][mx] = val
                seen.add(("h", y, x))
                seen.add(("h", my, mx))
            if ("v", y, x) not in seen:
                val = rng.random() < kelp_prob
                kelp_v[y][x] = val
                my, mx = (height - 1 - y) % height, (width - x) % width
                kelp_v[my][mx] = val
                seen.add(("v", y, x))
                seen.add(("v", my, mx))

    portal_partner: dict = {}
    next_id = 0
    attempts = 0
    placed = 0
    while placed < portal_pairs and attempts < portal_pairs * 50 + 50:
        attempts += 1
        axis = rng.choice(["h", "v"])
        y = rng.randrange(height)
        x = rng.randrange(width)
        if axis == "h":
            if kelp_h[y][x] or portal_h[y][x] >= 0:
                continue
            my, mx = (height - y) % height, (width - 1 - x) % width
            if kelp_h[my][mx] or portal_h[my][mx] >= 0:
                continue
            portal_h[y][x] = next_id
            portal_h[my][mx] = next_id
            portal_partner[("h", y, x)] = ("h", my, mx)
            portal_partner[("h", my, mx)] = ("h", y, x)
        else:
            if kelp_v[y][x] or portal_v[y][x] >= 0:
                continue
            my, mx = (height - 1 - y) % height, (width - x) % width
            if kelp_v[my][mx] or portal_v[my][mx] >= 0:
                continue
            portal_v[y][x] = next_id
            portal_v[my][mx] = next_id
            portal_partner[("v", y, x)] = ("v", my, mx)
            portal_partner[("v", my, mx)] = ("v", y, x)
        next_id += 1
        placed += 1

    game_map = GameMap(
        width, height, kelp_h, kelp_v, portal_h, portal_v, pearl_min, pearl_max,
        portal_partner,
    )

    # Clear kelp/portals around the centre line so spawn placement (done by
    # the caller, env.py) never starts a dragon boxed in.
    cx, cy = width // 2, height // 2
    for dy in range(-spawn_clear_radius, spawn_clear_radius + 1):
        for dx in range(-spawn_clear_radius, spawn_clear_radius + 1):
            x, y = (cx + dx) % width, (cy + dy) % height
            mx, my = _mirror_xy(width, height, x, y)
            for (xx, yy) in {(x, y), (mx, my)}:
                kelp_h[yy][xx] = False
                kelp_h[(yy + 1) % height][xx] = False
                kelp_v[yy][xx] = False
                kelp_v[yy][(xx + 1) % width] = False

    return game_map
