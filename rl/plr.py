"""Prioritized Level Replay (Jiang et al., 2021), over level seeds.

A level is a 64-bit seed: the C++ environment turns it into the same map and
match seed in any process (bccore.level_map), so replaying a level is just
asking for its seed again. Levels are scored by how much the learner still
has to learn on them (mean |GAE advantage| over their decisions) and replayed
by rank, mixed with a staleness term so no level's score goes too stale.
"""
from __future__ import annotations

import numpy as np


class LevelReplay:
    def __init__(self, rng: np.random.Generator, capacity: int = 2000, replay_prob: float = 0.5,
                 temperature: float = 0.3, staleness: float = 0.1, score_ema: float = 0.5, min_levels: int = 64):
        self.rng = rng
        self.capacity = capacity
        self.replay_prob = replay_prob
        self.temperature = temperature
        self.staleness = staleness
        self.score_ema = score_ema
        self.min_levels = min_levels
        self.seeds = np.zeros(capacity, dtype=np.uint64)
        self.scores = np.zeros(capacity, dtype=np.float64)
        self.last_seen = np.zeros(capacity, dtype=np.int64)
        self.size = 0
        self.index: dict[int, int] = {}
        self.clock = 0
        self.replayed = 0
        self.fresh = 0

    def fresh_seed(self) -> int:
        return int(self.rng.integers(0, 2 ** 63, dtype=np.int64)) | (int(self.rng.integers(0, 2)) << 63)

    def sample(self) -> int:
        self.clock += 1
        if self.size >= self.min_levels and self.rng.random() < self.replay_prob:
            weights = self._weights()
            slot = int(self.rng.choice(self.size, p=weights))
            self.last_seen[slot] = self.clock
            self.replayed += 1
            return int(self.seeds[slot])
        self.fresh += 1
        return self.fresh_seed()

    def _weights(self) -> np.ndarray:
        n = self.size
        order = np.argsort(-self.scores[:n], kind="stable")
        ranks = np.empty(n)
        ranks[order] = np.arange(1, n + 1)
        score_w = (1.0 / ranks) ** (1.0 / self.temperature)
        score_w /= score_w.sum()
        stale = (self.clock - self.last_seen[:n]).astype(np.float64)
        stale_w = stale / stale.sum() if stale.sum() > 0 else np.full(n, 1.0 / n)
        return (1 - self.staleness) * score_w + self.staleness * stale_w

    def update(self, seed: int, score: float):
        seed = int(seed)
        slot = self.index.get(seed)
        if slot is not None:
            self.scores[slot] = (1 - self.score_ema) * self.scores[slot] + self.score_ema * score
            self.last_seen[slot] = self.clock
            return
        if self.size < self.capacity:
            slot = self.size
            self.size += 1
        else:
            slot = int(np.argmin(self.scores[:self.size]))
            if self.scores[slot] >= score:
                return
            del self.index[int(self.seeds[slot])]
        self.seeds[slot] = np.uint64(seed)
        self.scores[slot] = score
        self.last_seen[slot] = self.clock
        self.index[seed] = slot

    def state_dict(self) -> dict:
        return {"seeds": self.seeds[:self.size].copy(), "scores": self.scores[:self.size].copy(),
                "last_seen": self.last_seen[:self.size].copy(), "clock": self.clock}

    def load_state_dict(self, state: dict):
        n = min(len(state["seeds"]), self.capacity)
        self.size = n
        self.seeds[:n] = state["seeds"][:n]
        self.scores[:n] = state["scores"][:n]
        self.last_seen[:n] = state["last_seen"][:n]
        self.clock = int(state["clock"])
        self.index = {int(s): i for i, s in enumerate(self.seeds[:n])}
