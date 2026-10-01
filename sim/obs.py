"""Observation encoding shared by the training environment (env.py, driven
off engine.py's `vision_window`) and the exported competition bot
(export/bot_template/main.py.tmpl, driven off the real `helper.py`'s
Controller/Tile API).

Both sides MUST produce the same feature vector given equivalent game state,
or a policy trained here will not play the same way once exported. They
can't share this module directly -- the exported bot ships as a standalone
zip with only the official helper and numpy available, no local imports --
so the layout is specified once here, as plain data, and both encoders are
written against it. tests/test_obs_contract.py cross-checks the two
implementations on synthetic fixtures.

Per-tile features (17), row-major over the 7x7 vision window:
  0  has_pearl
  1  pearl_countdown, clipped to [0, 64] and scaled to [0, 1] (0 if never-spawn)
  2  pearl_never_spawns (pearl_countdown == -1)
  3  edge N is kelp        4  edge E is kelp
  5  edge S is kelp        6  edge W is kelp
  7  edge N is portal      8  edge E is portal
  9  edge S is portal     10  edge W is portal
 11  self head             12  self body (non-head)
 13  ally head             14  ally body
 15  enemy head            16  enemy body

Scalar features (7), appended after the flattened 49*17 tile grid:
  own length, clipped to [0, 64] and scaled to [0, 1]
  own facing, one-hot over (N, E, S, W)
  own team's living unit count, scaled by 1/64
  round number, scaled by 1/500
"""
from __future__ import annotations

from . import constants as C

NUM_TILE_FEATURES = 17
VISION_TILES = C.VISION_SIZE * C.VISION_SIZE
OBS_SIZE = VISION_TILES * NUM_TILE_FEATURES + 7

_DIR_INDEX = {d: i for i, d in enumerate(C.DIRECTIONS)}


def encode_tile(tile: dict, team: str, self_id: int):
    feat = [0.0] * NUM_TILE_FEATURES
    feat[0] = 1.0 if tile["has_pearl"] else 0.0
    pt = tile["pearl_time"]
    if pt is not None and pt >= 0:
        feat[1] = min(pt, 64) / 64.0
    else:
        feat[2] = 1.0
    for i, d in enumerate(C.DIRECTIONS):
        etype, _ = tile["edges"][d]
        if etype == C.EDGE_KELP:
            feat[3 + i] = 1.0
        elif etype == C.EDGE_PORTAL:
            feat[7 + i] = 1.0
    part = tile["dragon"]
    if part is not None:
        mine = part["id"] == self_id
        same_team = part["team"] == team
        if part["is_head"]:
            feat[11] = 1.0 if mine else 0.0
            feat[13] = 1.0 if (same_team and not mine) else 0.0
            feat[15] = 1.0 if not same_team else 0.0
        else:
            feat[12] = 1.0 if mine else 0.0
            feat[14] = 1.0 if (same_team and not mine) else 0.0
            feat[16] = 1.0 if not same_team else 0.0
    return feat


def encode_observation(engine, dragon_id: int):
    """Builds the flat observation vector for `dragon_id` from an
    `sim.engine.Engine` instance. Returns a plain Python list of floats of
    length OBS_SIZE (callers vectorize with numpy/torch as needed)."""
    dragon = engine.dragons[dragon_id]
    window = engine.vision_window(dragon_id)
    feats = []
    for tile in window:
        feats.extend(encode_tile(tile, dragon.team, dragon_id))

    length_feat = min(dragon.length, 64) / 64.0
    facing = [0.0, 0.0, 0.0, 0.0]
    facing[_DIR_INDEX[dragon.facing]] = 1.0
    unit_count = engine.team_unit_count(dragon.team) / float(engine.unit_limit)
    # Normalized against the real game's fixed 500-round cap, not this
    # engine's (possibly shorter, for faster training episodes) max_rounds
    # -- otherwise this feature means something different at deploy time
    # than it did in training.
    round_frac = engine.round / float(C.MAX_ROUNDS)

    feats.append(length_feat)
    feats.extend(facing)
    feats.append(unit_count)
    feats.append(round_frac)
    assert len(feats) == OBS_SIZE
    return feats
