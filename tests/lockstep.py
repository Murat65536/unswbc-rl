"""Plays the same game on the real judge's engine and on ours, and records
everything a bot or the judge can observe, so the two can be compared exactly.

The real engine is `unswbc_engine.wasm` from the installed `unswbc` toolkit,
driven through the toolkit's own `unswbc.engine.EngineModule`. Ours is the
vendored engine in `engine/`, driven through `bccore.Match.run`. Both are fed
the same map text, the same 64-bit match seed and the same scripted players,
so with exact rules they produce the same stream of

    ("spawn", id, init_block)              a dragon's process starts
    ("turn", id, round_block, reply)       a dragon's turn, as its bot sees it
    ("death", id, round, reason)
    ("result", ...)                         the final score

Pearls are included: the seed decides every pearl countdown in both engines.
A scripted player decides only from its own round blocks and its own state,
exactly like a real bot (one process per dragon, no shared memory).
"""
from __future__ import annotations

import random
from dataclasses import dataclass, field

DIRS = "NESW"
STEP = {"N": (0, -1), "E": (1, 0), "S": (0, 1), "W": (-1, 0)}
LEFT = {"N": "W", "W": "S", "S": "E", "E": "N"}
RIGHT = {v: k for k, v in LEFT.items()}
BACK = {"N": "S", "S": "N", "E": "W", "W": "E"}


@dataclass
class Block:
    round: int
    facing: str
    length: int
    units: int
    messages: list
    echoes: list | None
    tiles: dict  # (x, y) -> (has_pearl, pearl_in)
    order: list  # tile coordinates row by row, as sent
    parts: dict  # (x, y) -> (team, id, facing, is_head)
    hedges: list  # 8 rows of 7
    vedges: list  # 7 rows of 8

    @property
    def head(self):
        return self.order[24]

    def edge(self, direction: str, col: int = 3, row: int = 3) -> str:
        """The edge on `direction`'s side of the window tile (col, row);
        the head is (3, 3)."""
        if direction == "N":
            return self.hedges[row][col]
        if direction == "S":
            return self.hedges[row + 1][col]
        if direction == "W":
            return self.vedges[row][col]
        return self.vedges[row][col + 1]

    def ahead(self, direction: str):
        """The tile one plain step away (None if beyond a portal or kelp)."""
        if self.edge(direction) != ".":
            return None
        dx, dy = STEP[direction]
        col, row = 3 + dx, 3 + dy
        return self.order[row * 7 + col]

    def clear_path(self, steps: str) -> bool:
        """Every step crosses an open edge onto a visible tile with no dragon
        on it (a conservative check: it ignores the tail moving away)."""
        col, row = 3, 3
        for d in steps:
            if self.edge(d, col, row) != ".":
                return False
            dx, dy = STEP[d]
            col, row = col + dx, row + dy
            if not (0 <= col < 7 and 0 <= row < 7) or self.order[row * 7 + col] in self.parts:
                return False
        return True


def parse_block(text: str) -> Block:
    lines = text.split("\n")
    at = 0

    def take():
        nonlocal at
        line = lines[at]
        at += 1
        return line

    round_num = int(take().split()[1])
    facing = take().split()[1]
    length = int(take().split()[1])
    units = int(take().split()[1])
    num_msgs = int(take().split()[1])
    messages = [int(take()) for _ in range(num_msgs)]
    echoes = None
    if lines[at].startswith("ECHOES"):
        echoes = [int(v) for v in take().split()[1:]]
    tiles, order = {}, []
    for _ in range(49):
        x, y, pearl, pearl_in = (int(v) for v in take().split())
        tiles[(x, y)] = (pearl == 1, pearl_in)
        order.append((x, y))
    parts = {}
    for _ in range(int(take().split()[1])):
        team, did, x, y, facing_, head = take().split()
        parts[(int(x), int(y))] = (team, int(did), facing_, head == "1")
    hedges = [take().split() for _ in range(8)]
    vedges = [take().split() for _ in range(7)]
    return Block(round_num, facing, length, units, messages, echoes, tiles, order, parts, hedges, vedges)


@dataclass
class Style:
    """How a scripted player behaves; every probability is per turn."""
    sprint: float = 0.15          # a 2-3 step move
    oversprint: float = 0.01      # a move it cannot pay for
    split: float = 0.05           # a legal split, when one exists
    bad_split: float = 0.005      # SPLIT 1, SPLIT L-1, SPLIT L, ...
    reckless: float = 0.05        # ignore safety entirely
    portal: float = 0.5           # step through a visible portal
    hunt: float = 0.2             # step onto an enemy head when one is adjacent
    sonar: float = 0.2
    protocol3: float = 0.02
    noise: float = 0.03           # garbage, repeats and debug lines
    silent: float = 0.002         # no action at all
    survive: bool = False         # only free, clear sprints; only long splits; no portals
    suicide_round: int = -1       # on this round the dragons named below turn back into themselves
    suicide_queen: bool = True    # the queen (dragon 0 or 1) if True, every other dragon if False


STYLES = {
    "careful": Style(sprint=0.1, oversprint=0.0, split=0.02, bad_split=0.0, reckless=0.0, noise=0.0, silent=0.0),
    "survivor": Style(sprint=0.5, oversprint=0.0, split=0.05, bad_split=0.0, reckless=0.0, portal=0.0, hunt=0.0,
                      noise=0.0, silent=0.0, survive=True),
    "mixed": Style(),
    "chaotic": Style(sprint=0.3, oversprint=0.05, split=0.1, bad_split=0.03, reckless=0.2, noise=0.2, silent=0.01),
    "splitter": Style(split=0.6, bad_split=0.02, sprint=0.05, reckless=0.0),
    "sprinter": Style(sprint=0.7, oversprint=0.05, split=0.02, reckless=0.02),
    # Survivors whose queen (or whose other dragons) die early, so that a
    # game reaching the round limit is decided by the queen tiebreak.
    "regicide": Style(sprint=0.5, oversprint=0.0, split=0.05, bad_split=0.0, reckless=0.0, portal=0.0, hunt=0.0,
                      noise=0.0, silent=0.0, survive=True, suicide_round=20),
    "loyal": Style(sprint=0.5, oversprint=0.0, split=0.05, bad_split=0.0, reckless=0.0, portal=0.0, hunt=0.0,
                   noise=0.0, silent=0.0, survive=True, suicide_round=20, suicide_queen=False),
}


class Player:
    """One dragon's scripted bot. Deterministic given its seed and its blocks."""

    def __init__(self, seed: int, style: Style, did: int = -1):
        self.rng = random.Random(seed)
        self.style = style
        self.did = did

    def reply(self, text: str) -> str:
        block = parse_block(text)
        s, rng = self.style, self.rng
        out = []
        if rng.random() < s.noise:
            out.append(rng.choice([
                "LOG hello there", "INDICATOR busy", "DOT 1 2 255 0 0", "LINE 0 0 3 4 0 255 0",
                "MOVE", "MOVE NX", "move N", "SPLIT", "SPLIT x", "SONAR", "SONAR N", "SONAR 18446744073709551616",
                "PROTOCOL", "WHAT", "   ", "MOVE N E", f"MOVE {rng.choice(DIRS)}",
            ]))
        if rng.random() < s.silent:
            return "\n".join(out + ["ENDTURN"]) + "\n"

        out.append(self.action(block))
        if rng.random() < s.noise:
            out.append(f"MOVE {rng.choice(DIRS)}")  # the last action wins
        if rng.random() < s.sonar:
            for d in DIRS:
                if rng.random() < 0.4:
                    value = rng.choice([rng.getrandbits(16), rng.getrandbits(32), rng.getrandbits(64)])
                    out.append(f"SONAR {d} {value}")
            if rng.random() < 0.3:
                out.append(f"SONAR {rng.getrandbits(32)}")
        if rng.random() < s.protocol3:
            out.append("PROTOCOL 3")
        return "\n".join(out + ["ENDTURN"]) + "\n"

    def action(self, block: Block) -> str:
        s, rng = self.style, self.rng
        length = block.length
        if rng.random() < s.bad_split:
            return f"SPLIT {rng.choice([0, 1, length - 1, length, length + 3, -2])}"
        if block.round == s.suicide_round and (self.did <= 1) == s.suicide_queen:
            return "MOVE " + BACK[block.facing]
        if s.survive:
            return self.survive(block)
        if length >= 4 and rng.random() < s.split:
            return f"SPLIT {rng.randint(2, length - 2)}"

        safe, pearl, portal, hunt = [], [], [], []
        mine = block.parts.get(block.head)
        for d in DIRS:
            if d == BACK[block.facing]:
                continue
            edge = block.edge(d)
            if edge == "w":
                continue
            if edge != ".":
                portal.append(d)
                continue
            tile = block.ahead(d)
            part = block.parts.get(tile)
            if part is not None:
                if part[3] and mine is not None and part[0] != mine[0]:
                    hunt.append(d)
                continue
            safe.append(d)
            if block.tiles[tile][0]:
                pearl.append(d)

        if rng.random() < s.reckless:
            first = rng.choice(DIRS)
        elif hunt and rng.random() < s.hunt:
            first = rng.choice(hunt)
        elif portal and rng.random() < s.portal:
            first = rng.choice(portal)
        elif pearl:
            first = rng.choice(pearl)
        elif safe:
            first = rng.choice(safe)
        elif portal:
            first = rng.choice(portal)
        else:
            first = rng.choice(DIRS)

        steps = [first]
        if rng.random() < s.oversprint:
            steps += [rng.choice([first, LEFT[first], RIGHT[first]]) for _ in range(rng.randint(length, length + 3))]
        elif rng.random() < s.sprint:
            # Mostly sprints it can pay for: ceil(L / 4) free steps, then one
            # segment a step down to length 2.
            affordable = min(3, (length + 3) // 4 + length - 2)
            for _ in range(rng.randint(1, affordable - 1) if affordable >= 2 else 0):
                last = steps[-1]
                steps.append(rng.choice([last, last, LEFT[last], RIGHT[last]]))
        return "MOVE " + "".join(steps)

    def survive(self, block: Block) -> str:
        s, rng = self.style, self.rng
        length = block.length
        if length >= 8 and rng.random() < s.split:
            return f"SPLIT {rng.randint(4, length - 4)}"
        moves = [d for d in DIRS if d != BACK[block.facing] and block.clear_path(d)]
        if not moves:
            return "MOVE " + rng.choice([d for d in DIRS if d != BACK[block.facing]])
        pearls = [d for d in moves if block.tiles[block.ahead(d)][0]]
        first = rng.choice(pearls or moves)
        free = (length + 3) // 4
        if free >= 2 and rng.random() < s.sprint:
            seconds = [d for d in (first, LEFT[first], RIGHT[first]) if block.clear_path(first + d)]
            if seconds:
                return "MOVE " + first + rng.choice(seconds)
        return "MOVE " + first


@dataclass
class Recording:
    stream: list = field(default_factory=list)
    result: dict | None = None


class Players:
    """A fresh Player per dragon process, seeded from the game and the id."""

    def __init__(self, game_seed: int, styles: tuple[str, str]):
        self.game_seed = game_seed
        self.styles = styles
        self.players: dict[int, Player] = {}
        self.teams: dict[int, str] = {}

    def spawn(self, did: int, init: str):
        team = next(line.split()[1] for line in init.splitlines() if line.startswith("TEAM"))
        self.teams[did] = team
        style = STYLES[self.styles[0 if team == "A" else 1]]
        self.players[did] = Player(self.game_seed * 1_000_003 + did, style, did)

    def reply(self, did: int, block: str) -> str:
        return self.players[did].reply(block)


def _result_tuple(result: dict) -> tuple:
    return tuple(result[k] for k in ("rounds", "winner", "end_reason", "a_dragons", "b_dragons", "a_length",
                                      "b_length", "a_queen", "b_queen", "a_longest", "b_longest"))


def play_ours(map_text: str, seed: int, styles: tuple[str, str]) -> Recording:
    import bccore

    rec = Recording()
    players = Players(seed, styles)

    def spawn(did, init):
        rec.stream.append(("spawn", did, init))
        players.spawn(did, init)

    def reply(did, block):
        out = players.reply(did, block)
        rec.stream.append(("turn", did, block, out))
        return out

    def death(did, round_num, reason):
        rec.stream.append(("death", did, round_num, reason))

    result = bccore.Match(map_text, seed).run(reply, spawn, death)
    rec.result = _result_tuple(result)
    return rec


_ENGINE = None


def judge_engine():
    global _ENGINE
    if _ENGINE is None:
        from unswbc.engine import EngineModule

        _ENGINE = EngineModule()
    return _ENGINE


def play_judge(map_text: str, seed: int, styles: tuple[str, str]) -> Recording:
    from unswbc.engine import DEBUG_ALL

    rec = Recording()
    players = Players(seed, styles)

    def spawn(did, init):
        init = init.decode()
        rec.stream.append(("spawn", did, init))
        players.spawn(did, init)

    def reply(did, block):
        block = block.decode()
        out = players.reply(did, block)
        rec.stream.append(("turn", did, block, out))
        return out.encode()

    def death(did, round_num, reason):
        rec.stream.append(("death", did, round_num, reason))

    notices = []
    result = judge_engine().run(map_text.encode(), reply, death, spawn, notices.append, DEBUG_ALL, seed)
    assert not notices, f"the judge corrected the map: {notices[:3]}"
    rec.result = (result.rounds, result.winner, result.end_reason, result.a_dragons, result.b_dragons,
                  result.a_length, result.b_length, result.a_queen, result.b_queen, result.a_longest,
                  result.b_longest)
    return rec


def first_difference(ours: Recording, judge: Recording) -> str | None:
    """None if the two games are identical, else a readable account of where
    they first part ways."""
    for i, (a, b) in enumerate(zip(ours.stream, judge.stream)):
        if a != b:
            context = judge.stream[max(0, i - 2):i]
            return (f"event {i} differs\n  ours:  {a!r}\n  judge: {b!r}\n"
                    f"  preceded by: {context!r}")
    if len(ours.stream) != len(judge.stream):
        n = min(len(ours.stream), len(judge.stream))
        return (f"stream lengths differ: ours {len(ours.stream)}, judge {len(judge.stream)}; next: "
                f"ours {ours.stream[n:n + 1]!r} judge {judge.stream[n:n + 1]!r}")
    if ours.result != judge.result:
        return f"results differ: ours {ours.result}, judge {judge.result}"
    return None


def effective_action(reply: str):
    """The action the engine takes from a reply: the last readable MOVE or
    SPLIT line before ENDTURN, as ("move", steps), ("split", n) or None."""
    action = None
    for line in reply.split("\n"):
        words = line.split()
        if words == ["ENDTURN"]:
            break
        if len(words) == 2 and words[0] == "MOVE" and words[1] and set(words[1]) <= set(DIRS):
            action = ("move", words[1])
        elif len(words) == 2 and words[0] == "SPLIT":
            try:
                action = ("split", int(words[1]))
            except ValueError:
                pass
    return action


def coverage(rec: Recording) -> dict:
    """Counts the rule situations a game exercised, so a test can insist that
    the lockstep games are not vacuous."""
    from collections import Counter

    seen = Counter()
    limits = {}
    last_block = {}
    born = {}
    for i, event in enumerate(rec.stream):
        kind = event[0]
        if kind == "spawn":
            did, init = event[1], event[2]
            limits[did] = int(next(l.split()[1] for l in init.splitlines() if l.startswith("UNIT_LIMIT")))
            if last_block:
                born[did] = current_round
        elif kind == "death":
            seen[f"death_{event[3]}"] += 1
        elif kind == "turn":
            did, text, reply = event[1], event[2], event[3]
            block = parse_block(text)
            current_round = block.round
            if did in born:
                if born.pop(did) == block.round:
                    seen["child_acts_in_birth_round"] += 1
            prev = last_block.get(did)
            if prev is not None and block.length > prev.length:
                seen["pearl_eaten"] += 1
            last_block[did] = block
            if block.messages:
                seen["sonar_received"] += 1
                if max(block.messages) > 0xFFFFFFFF:
                    seen["sonar_64bit_received"] += 1
            if block.echoes is not None and any(block.echoes):
                seen["sonar_echoes"] += 1
            action = effective_action(reply)
            length, free = block.length, (block.length + 3) // 4
            if action is None:
                seen["no_action"] += 1
            elif action[0] == "move":
                steps = action[1]
                if block.edge(steps[0]) not in (".", "w"):
                    seen["portal_step"] += 1
                if len(steps) > 1 and len(steps) <= free:
                    seen["free_sprint"] += 1
                if free < len(steps) <= free + length - 2:
                    seen["paid_sprint"] += 1
                if len(steps) > free + length - 2:
                    seen["unpayable_sprint"] += 1
            else:
                n = action[1]
                if not (2 <= n <= length - 2):
                    seen["illegal_split"] += 1
                elif block.units >= limits[did]:
                    seen["split_at_unit_limit"] += 1
                else:
                    seen["legal_split"] += 1
    result = dict(zip(("rounds", "winner", "end_reason", "a_dragons", "b_dragons", "a_length", "b_length",
                       "a_queen", "b_queen", "a_longest", "b_longest"), rec.result))
    if result["end_reason"] == 0:
        seen["ended_by_elimination" if result["winner"] else "ended_both_eliminated"] += 1
    else:
        seen["ended_at_round_limit"] += 1
        if result["a_queen"] != result["b_queen"]:
            seen["round_limit_queen_decides"] += 1
        elif result["winner"] is not None:
            seen["round_limit_later_tiebreak"] += 1
    return seen


def ring_map(size: int = 14) -> str:
    """A hand-built xy-symmetric map where every dragon sits in its own sealed
    ring of eight tiles with no pearl beds. A dragon that keeps moving forward
    circles its ring forever without growing, so the game reaches the round
    limit with exactly the dragons whose players chose to die gone: the
    queens are length 3 and the other two dragons length 4, which makes the
    queen tiebreak decide games that longest-then-total would decide the
    other way."""
    w = h = size
    kelp = set()  # ("h" | "v", x, y): the north / west side of tile (x, y)
    corners = [(2, 2), (8, 2)]
    corners += [(w - 3 - x0, h - 3 - y0) for x0, y0 in corners]
    rings = set()
    for x0, y0 in corners:
        for i in range(3):
            kelp.update({("h", x0 + i, y0), ("h", x0 + i, y0 + 3), ("v", x0, y0 + i), ("v", x0 + 3, y0 + i)})
        kelp.update({("h", x0 + 1, y0 + 1), ("h", x0 + 1, y0 + 2), ("v", x0 + 1, y0 + 1), ("v", x0 + 2, y0 + 1)})
        rings.update((x, y) for x in range(x0, x0 + 3) for y in range(y0, y0 + 3))

    lines = [f"MAP {w} {h}", "SYMMETRY xy", "MAP_NAME rings"]
    tiles = []
    for y in range(h):
        for x in range(w):
            if (x, y) not in rings:
                key = min((x, y), (w - 1 - x, h - 1 - y))
                tiles.append(f"TILE {x} {y} {1 + (key[0] * 7 + key[1] * 3) % 5} 40")
    lines += [f"TILE_COUNT {len(tiles)}"] + tiles
    edges = sorted((2 * y if axis == "h" else 2 * y + 1) * (w + 1) + x for axis, x, y in kelp)
    lines += [f"EDGE_COUNT {len(edges)}"] + [f"EDGE {e} 1 -1" for e in edges]

    def mirror(body):
        return [(w - 1 - x, h - 1 - y) for x, y in body]

    queen = [(4, 2), (3, 2), (2, 2)]
    other = [(10, 2), (9, 2), (8, 2), (8, 3)]
    lines.append("DRAGON_COUNT 4")
    for team, body in ((0, queen), (1, mirror(queen)), (0, other), (1, mirror(other))):
        lines.append(f"DRAGON {team} {len(body)} " + " ".join(f"{x} {y}" for x, y in body))
    return "\n".join(lines) + "\n"
