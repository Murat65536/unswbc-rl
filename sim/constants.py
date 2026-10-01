"""Constants mirroring the UNSW Battlecode 2026 "Dragons" ruleset.

Sourced from docs/rules/*.md (scraped from game.battlecode.au/docs/overview).
"""

DIRECTIONS = ("N", "E", "S", "W")

# (dx, dy); y grows downwards, matching the real game's coordinate system.
OFFSET = {
    "N": (0, -1),
    "E": (1, 0),
    "S": (0, 1),
    "W": (-1, 0),
}

OPPOSITE = {"N": "S", "S": "N", "E": "W", "W": "E"}
LEFT = {"N": "W", "W": "S", "S": "E", "E": "N"}
RIGHT = {v: k for k, v in LEFT.items()}

EDGE_EMPTY = 0
EDGE_KELP = 1
EDGE_PORTAL = 2

MAX_ROUNDS = 500
VISION_RADIUS = 3  # 7x7 window, Chebyshev distance <= 3
VISION_SIZE = 2 * VISION_RADIUS + 1

SPAWN_LENGTH = 3
MIN_LENGTH = 2
DEFAULT_UNIT_LIMIT = 64

# Death reasons
DEATH_WALL = "hit_wall"  # moved into kelp
DEATH_SELF = "hit_self"
DEATH_OTHER_BODY = "hit_other_body"
DEATH_HEAD_TO_HEAD = "head_to_head"
DEATH_NO_VALID_ACTION = "no_valid_action"
