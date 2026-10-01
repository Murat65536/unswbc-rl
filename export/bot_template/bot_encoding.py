"""Observation encoding and move-safety checks for the exported bot,
written against the real `helper.py` API.

This MUST mirror sim/obs.py's layout exactly -- tests/test_obs_contract.py
(in the training repo, not shipped with the bot) checks the two agree on
synthetic fixtures. It's a separate file from main.py (rather than inlined)
specifically so that contract test can import it directly, without also
triggering main.py's weight loading.
"""
from helper import Direction, EdgeType

DIRS = ["N", "E", "S", "W"]
DIR_ENUM = {"N": Direction.NORTH, "E": Direction.EAST, "S": Direction.SOUTH, "W": Direction.WEST}
OBS_SIZE = 840
MAX_ROUNDS = 500


def encode_tile(tile, my_id: int, my_team) -> list:
    feat = [0.0] * 17
    feat[0] = 1.0 if tile.has_pearl() else 0.0
    pt = tile.get_pearl_time()
    if pt >= 0:
        feat[1] = min(pt, 64) / 64.0
    else:
        feat[2] = 1.0
    for i, d in enumerate(DIRS):
        etype = tile.get_edge(DIR_ENUM[d]).get_edge_type()
        if etype == EdgeType.KELP:
            feat[3 + i] = 1.0
        elif etype == EdgeType.PORTAL:
            feat[7 + i] = 1.0
    part = tile.get_dragon()
    if part is not None:
        mine = part.get_id() == my_id
        same_team = part.get_team() == my_team
        if part.is_head():
            feat[11] = 1.0 if mine else 0.0
            feat[13] = 1.0 if (same_team and not mine) else 0.0
            feat[15] = 1.0 if not same_team else 0.0
        else:
            feat[12] = 1.0 if mine else 0.0
            feat[14] = 1.0 if (same_team and not mine) else 0.0
            feat[16] = 1.0 if not same_team else 0.0
    return feat


def encode_observation(ct, game) -> list:
    feats = []
    my_id = ct.get_id()
    my_team = ct.get_team()
    for tile in ct.get_tiles():
        feats.extend(encode_tile(tile, my_id, my_team))

    feats.append(min(ct.get_length(), 64) / 64.0)
    facing = [0.0, 0.0, 0.0, 0.0]
    facing[DIRS.index(ct.get_dir().value)] = 1.0
    feats.extend(facing)
    feats.append(ct.get_unit_count() / float(game.get_unit_limit()))
    feats.append(game.get_round_num() / float(MAX_ROUNDS))
    assert len(feats) == OBS_SIZE
    return feats


def safe_directions(ct) -> list:
    here = ct.get_position()
    here_tile = ct.get_tile(here)
    safe = []
    for d in DIRS:
        direction = DIR_ENUM[d]
        if here_tile.get_edge(direction).get_edge_type() == EdgeType.KELP:
            continue
        ahead = ct.get_tile(here.add_dir(direction))
        part = ahead.get_dragon() if ahead is not None else None
        if part is not None and not part.is_head():
            continue  # certain death: our own or someone else's non-head body
        safe.append(d)
    return safe
