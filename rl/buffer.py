"""Per-agent trajectory bookkeeping and GAE for PPO.

Dragons die at different times and episodes end asynchronously across
vectorized envs, so there's no single fixed-length tensor of experience the
way there is for a single-agent env. Instead, each (env, living dragon)
pair gets its own small trajectory buffer; when the dragon dies, the game
ends, or the rollout's time limit is hit (truncation, bootstrapped with the
critic), that buffer is finalized into a `Trajectory` and GAE is computed
over just that buffer's own steps.
"""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np


@dataclass
class Trajectory:
    obs: np.ndarray
    actions: np.ndarray
    logprobs: np.ndarray
    values: np.ndarray
    returns: np.ndarray
    advantages: np.ndarray


@dataclass
class TrajBuffer:
    last_obs: list
    obs: list = field(default_factory=list)
    actions: list = field(default_factory=list)
    logprobs: list = field(default_factory=list)
    values: list = field(default_factory=list)
    rewards: list = field(default_factory=list)

    def record(self, obs, action, logprob, value, reward):
        self.obs.append(obs)
        self.actions.append(action)
        self.logprobs.append(logprob)
        self.values.append(value)
        self.rewards.append(reward)

    def __len__(self):
        return len(self.obs)

    def finalize(self, bootstrap_value: float, gamma: float, gae_lambda: float) -> Trajectory | None:
        n = len(self.obs)
        if n == 0:
            return None
        values = np.asarray(self.values + [bootstrap_value], dtype=np.float32)
        rewards = np.asarray(self.rewards, dtype=np.float32)
        advantages = np.zeros(n, dtype=np.float32)
        gae = 0.0
        for t in reversed(range(n)):
            delta = rewards[t] + gamma * values[t + 1] - values[t]
            gae = delta + gamma * gae_lambda * gae
            advantages[t] = gae
        returns = advantages + values[:-1]
        return Trajectory(
            obs=np.asarray(self.obs, dtype=np.float32),
            actions=np.asarray(self.actions, dtype=np.int64),
            logprobs=np.asarray(self.logprobs, dtype=np.float32),
            values=values[:-1],
            returns=returns,
            advantages=advantages,
        )


def concat_trajectories(trajs: list[Trajectory]):
    return (
        np.concatenate([t.obs for t in trajs]),
        np.concatenate([t.actions for t in trajs]),
        np.concatenate([t.logprobs for t in trajs]),
        np.concatenate([t.values for t in trajs]),
        np.concatenate([t.returns for t in trajs]),
        np.concatenate([t.advantages for t in trajs]),
    )
