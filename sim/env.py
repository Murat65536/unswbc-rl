"""A multi-agent RL environment around engine.Engine.

API is the common "parallel" multi-agent shape (à la PettingZoo /
RLlib's ParallelEnv): reset() returns {agent_id: obs}, step(actions) returns
(obs, rewards, dones, infos) dicts keyed by agent id, plus a "__all__" key in
`dones` saying whether the whole episode has ended.

Agent ids are dragon ids and are stable for an episode (no splitting in this
version, so the agent set only shrinks as dragons die -- see README's
"Known limitations").
"""
from __future__ import annotations

import random

from . import constants as C
from .engine import Dragon, Engine
from .map import random_symmetric_map
from .obs import OBS_SIZE, encode_observation


def default_reward_config():
    return dict(
        pearl_reward=1.0,
        alive_bonus=0.01,
        death_penalty=-10.0,
        win_bonus=5.0,
        loss_penalty=-5.0,
    )


def _clear_cell_edges(game_map, x, y):
    w, h = game_map.width, game_map.height
    game_map.kelp_h[y][x] = False
    game_map.kelp_h[(y + 1) % h][x] = False
    game_map.kelp_v[y][x] = False
    game_map.kelp_v[y][(x + 1) % w] = False


class BattlecodeMultiAgentEnv:
    def __init__(self, width=16, height=16, team_size=2, max_rounds=300,
                 kelp_prob=0.05, portal_pairs=0, reward_cfg=None, seed=None):
        self.width = width
        self.height = height
        self.team_size = team_size
        self.max_rounds = max_rounds
        self.kelp_prob = kelp_prob
        self.portal_pairs = portal_pairs
        self.reward_cfg = reward_cfg or default_reward_config()
        self.rng = random.Random(seed)
        self.engine: Engine | None = None

    @property
    def observation_size(self):
        return OBS_SIZE

    @property
    def num_actions(self):
        return len(C.DIRECTIONS)

    def _spawn_dragons(self):
        w, h = self.width, self.height
        dragons = []
        spawn_rows = [
            (h // 2 + off) % h
            for off in range(-(self.team_size // 2), self.team_size - self.team_size // 2)
        ]
        for k, ay in enumerate(spawn_rows):
            ax = 3 + 3 * (k % max(1, w // 6))
            by = h - 1 - ay
            bx = w - 1 - ax
            a_body = [((ax - i) % w, ay) for i in range(C.SPAWN_LENGTH)]
            b_body = [((bx + i) % w, by) for i in range(C.SPAWN_LENGTH)]
            dragons.append(Dragon(id=2 * k, team="A", body=a_body))
            dragons.append(Dragon(id=2 * k + 1, team="B", body=b_body))
        return dragons

    def reset(self, seed=None):
        if seed is not None:
            self.rng = random.Random(seed)
        game_map = random_symmetric_map(
            self.width, self.height, self.rng,
            kelp_prob=self.kelp_prob, portal_pairs=self.portal_pairs,
        )
        dragons = self._spawn_dragons()
        for d in dragons:
            for (x, y) in d.body:
                _clear_cell_edges(game_map, x, y)
        engine_rng = random.Random(self.rng.randint(0, 2 ** 31 - 1))
        self.engine = Engine(game_map, dragons, engine_rng, max_rounds=self.max_rounds)
        return {
            did: encode_observation(self.engine, did)
            for did, d in self.engine.dragons.items() if d.alive
        }

    def step(self, actions: dict):
        engine = self.engine
        prev_alive = {did for did, d in engine.dragons.items() if d.alive}
        prev_length = {did: engine.dragons[did].length for did in prev_alive}

        engine.step(actions)

        cfg = self.reward_cfg
        game_over = engine.done
        obs, rewards, dones, infos = {}, {}, {}, {}
        for did in prev_alive:
            d = engine.dragons[did]
            r = 0.0
            if d.alive:
                r += (d.length - prev_length[did]) * cfg["pearl_reward"]
                r += cfg["alive_bonus"]
            else:
                r += cfg["death_penalty"]
            if game_over and engine.winner is not None:
                r += cfg["win_bonus"] if engine.winner == d.team else cfg["loss_penalty"]
            rewards[did] = r
            done = (not d.alive) or game_over
            dones[did] = done
            infos[did] = {"length": d.length, "alive": d.alive, "death_reason": d.death_reason}
            if d.alive and not game_over:
                obs[did] = encode_observation(engine, did)
        dones["__all__"] = game_over
        dones["__winner__"] = engine.winner if game_over else None
        return obs, rewards, dones, infos
