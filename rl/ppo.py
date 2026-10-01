"""Self-play PPO over a vectorized set of BattlecodeMultiAgentEnv games.

Every living dragon, on both teams, across every parallel env, is an
independent agent sharing one policy (see policy.py). Rollout collection
batches all of them into a single forward pass per timestep (good GPU
utilization even though each individual env is simulated on CPU), and each
agent's own (possibly short, death-truncated) trajectory is turned into a
GAE advantage estimate independently.
"""
from __future__ import annotations

from collections import defaultdict

import numpy as np
import torch
import torch.nn as nn
from torch.distributions import Categorical

from sim import constants as C
from .buffer import TrajBuffer, concat_trajectories


class PPOTrainer:
    def __init__(self, vec_env, policy, device="cpu", gamma=0.99, gae_lambda=0.95,
                 lr=3e-4, clip=0.2, vf_coef=0.5, ent_coef=0.01, max_grad_norm=0.5,
                 epochs=4, minibatch_size=2048, seed=0):
        self.vec_env = vec_env
        self.policy = policy.to(device)
        self.device = device
        self.gamma = gamma
        self.gae_lambda = gae_lambda
        self.clip = clip
        self.vf_coef = vf_coef
        self.ent_coef = ent_coef
        self.max_grad_norm = max_grad_norm
        self.epochs = epochs
        self.minibatch_size = minibatch_size
        self.optimizer = torch.optim.Adam(self.policy.parameters(), lr=lr)

        rng = np.random.default_rng(seed)
        seeds = [int(s) for s in rng.integers(0, 2 ** 31 - 1, size=vec_env.num_envs)]
        obs_per_env = vec_env.reset(seeds)
        self.active = [
            {did: TrajBuffer(last_obs=o) for did, o in obs.items()}
            for obs in obs_per_env
        ]
        self._reset_seed_rng = rng
        self.total_steps = 0

    def _policy_forward(self, flat_obs):
        obs_tensor = torch.as_tensor(np.asarray(flat_obs, dtype=np.float32), device=self.device)
        with torch.no_grad():
            logits, values = self.policy(obs_tensor)
            dist = Categorical(logits=logits)
            actions = dist.sample()
            logprobs = dist.log_prob(actions)
        return actions.cpu().numpy(), logprobs.cpu().numpy(), values.cpu().numpy()

    def collect_rollout(self, rollout_length: int):
        trajectories = []
        stats = defaultdict(list)

        for _ in range(rollout_length):
            flat_obs, key_list = [], []
            for env_idx, agents in enumerate(self.active):
                for did, buf in agents.items():
                    flat_obs.append(buf.last_obs)
                    key_list.append((env_idx, did))
            if not flat_obs:
                break

            actions_np, logprobs_np, values_np = self._policy_forward(flat_obs)

            actions_per_env = [dict() for _ in range(self.vec_env.num_envs)]
            pending = {}
            for (env_idx, did), a, lp, v, o in zip(key_list, actions_np, logprobs_np, values_np, flat_obs):
                actions_per_env[env_idx][did] = C.DIRECTIONS[int(a)]
                pending[(env_idx, did)] = (o, int(a), float(lp), float(v))

            step_results = self.vec_env.step(actions_per_env)
            self.total_steps += len(flat_obs)

            for env_idx, (obs_out, rewards, dones, infos) in enumerate(step_results):
                agents = self.active[env_idx]
                for did in list(agents.keys()):
                    o, a, lp, v = pending[(env_idx, did)]
                    buf = agents[did]
                    buf.record(o, a, lp, v, rewards[did])
                    if dones[did]:
                        traj = buf.finalize(0.0, self.gamma, self.gae_lambda)
                        if traj is not None:
                            trajectories.append(traj)
                        stats["episode_reward"].append(float(sum(buf.rewards)))
                        stats["episode_length"].append(len(buf))
                        stats["final_dragon_length"].append(infos[did]["length"])
                        del agents[did]
                    else:
                        buf.last_obs = obs_out[did]
                if dones["__all__"]:
                    winner = dones.get("__winner__")
                    stats["game_winner"].append(winner if winner is not None else "draw")
                    seed = int(self._reset_seed_rng.integers(0, 2 ** 31 - 1))
                    new_obs = self.vec_env.reset_one(env_idx, seed=seed)
                    self.active[env_idx] = {did: TrajBuffer(last_obs=o) for did, o in new_obs.items()}

        # Truncate still-active trajectories at the rollout boundary, bootstrapping
        # with the critic's value estimate (standard PPO time-limit handling).
        flat_obs, key_list = [], []
        for env_idx, agents in enumerate(self.active):
            for did, buf in agents.items():
                if len(buf) > 0:
                    flat_obs.append(buf.last_obs)
                    key_list.append((env_idx, did))
        if flat_obs:
            obs_tensor = torch.as_tensor(np.asarray(flat_obs, dtype=np.float32), device=self.device)
            with torch.no_grad():
                _, values = self.policy(obs_tensor)
            values_np = values.cpu().numpy()
            for (env_idx, did), v in zip(key_list, values_np):
                buf = self.active[env_idx][did]
                traj = buf.finalize(float(v), self.gamma, self.gae_lambda)
                if traj is not None:
                    trajectories.append(traj)
                self.active[env_idx][did] = TrajBuffer(last_obs=buf.last_obs)

        return trajectories, stats

    def update(self, trajectories):
        obs, actions, old_logprobs, _old_values, returns, advantages = concat_trajectories(trajectories)
        advantages = (advantages - advantages.mean()) / (advantages.std() + 1e-8)

        obs_t = torch.as_tensor(obs, device=self.device)
        actions_t = torch.as_tensor(actions, device=self.device)
        old_logprobs_t = torch.as_tensor(old_logprobs, device=self.device)
        returns_t = torch.as_tensor(returns, device=self.device)
        advantages_t = torch.as_tensor(advantages, device=self.device)

        n = obs_t.shape[0]
        stats = defaultdict(list)
        for _ in range(self.epochs):
            idx = np.random.permutation(n)
            for start in range(0, n, self.minibatch_size):
                mb = torch.as_tensor(idx[start:start + self.minibatch_size], device=self.device)
                logits, values = self.policy(obs_t[mb])
                dist = Categorical(logits=logits)
                logprobs = dist.log_prob(actions_t[mb])
                entropy = dist.entropy().mean()

                ratio = torch.exp(logprobs - old_logprobs_t[mb])
                surr1 = ratio * advantages_t[mb]
                surr2 = torch.clamp(ratio, 1 - self.clip, 1 + self.clip) * advantages_t[mb]
                policy_loss = -torch.min(surr1, surr2).mean()
                value_loss = 0.5 * ((values - returns_t[mb]) ** 2).mean()
                loss = policy_loss + self.vf_coef * value_loss - self.ent_coef * entropy

                self.optimizer.zero_grad()
                loss.backward()
                nn.utils.clip_grad_norm_(self.policy.parameters(), self.max_grad_norm)
                self.optimizer.step()

                stats["policy_loss"].append(policy_loss.item())
                stats["value_loss"].append(value_loss.item())
                stats["entropy"].append(entropy.item())
        return stats
