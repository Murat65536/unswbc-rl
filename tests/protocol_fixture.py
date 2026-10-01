"""Serializes a sim.engine.Engine state into the real wire protocol text,
so tests can feed it to the official helper.py and compare against our own
training-side observation encoder (sim/obs.py). See test_obs_contract.py.
"""
from sim import constants as C


def build_init_text(dragon, game_map) -> str:
    return (
        f"ID {dragon.id}\n"
        f"TEAM {dragon.team}\n"
        f"MAP {game_map.width} {game_map.height}\n"
        f"UNIT_LIMIT {C.DEFAULT_UNIT_LIMIT}\n"
    )


def _edge_symbol(engine, x, y, direction):
    etype, portal_id = engine.map.edge(x, y, direction)
    if etype == C.EDGE_KELP:
        return "w"
    if etype == C.EDGE_PORTAL:
        return str(portal_id)
    return "."


def build_round_text(engine, dragon_id: int) -> str:
    dragon = engine.dragons[dragon_id]
    hx, hy = dragon.head
    r = C.VISION_RADIUS

    lines = [
        f"ROUND {engine.round}",
        f"DIR {dragon.facing}",
        f"LENGTH {dragon.length}",
        f"UNIT_COUNT {engine.team_unit_count(dragon.team)}",
        "NUM_MSGS 0",
    ]

    window = engine.vision_window(dragon_id)
    for tile in window:
        x, y = tile["pos"]
        has_pearl = 1 if tile["has_pearl"] else 0
        lines.append(f"{x} {y} {has_pearl} {tile['pearl_time']}")

    body_lines = []
    for tile in window:
        part = tile["dragon"]
        if part is None:
            continue
        x, y = tile["pos"]
        body_lines.append(f"{part['team']} {part['id']} {x} {y} {part['dir']} {1 if part['is_head'] else 0}")
    lines.append(f"DRAGON_BODIES {len(body_lines)}")
    lines.extend(body_lines)

    for row in range(C.VISION_SIZE + 1):
        y = hy - r + row
        lines.append(" ".join(_edge_symbol(engine, hx - r + col, y, "N") for col in range(C.VISION_SIZE)))
    for row in range(C.VISION_SIZE):
        y = hy - r + row
        lines.append(" ".join(_edge_symbol(engine, hx - r + col, y, "W") for col in range(C.VISION_SIZE + 1)))

    return "\n".join(lines) + "\n"
