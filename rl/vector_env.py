"""Runs several BattlecodeMultiAgentEnv instances side by side.

Two implementations share one interface (`reset`, `step`, `num_envs`):

- `SerialVecEnv` steps every env in a plain Python loop, in-process. Simple
  to debug, and on Colab's 2 vCPUs the GIL means a process pool buys little
  anyway, so this is the default.
- `SubprocVecEnv` runs each env in its own process via `multiprocessing`,
  for use on machines with more cores (or local development).
"""
from __future__ import annotations

import multiprocessing as mp


class SerialVecEnv:
    def __init__(self, env_fns):
        self.envs = [fn() for fn in env_fns]

    @property
    def num_envs(self):
        return len(self.envs)

    def reset(self, seeds=None):
        if seeds is None:
            return [env.reset() for env in self.envs]
        return [env.reset(seed=s) for env, s in zip(self.envs, seeds)]

    def reset_one(self, idx, seed=None):
        return self.envs[idx].reset(seed=seed)

    def step(self, actions_per_env):
        return [env.step(a) for env, a in zip(self.envs, actions_per_env)]

    def close(self):
        pass


def _worker(remote, env_fn):
    env = env_fn()
    while True:
        cmd, data = remote.recv()
        if cmd == "step":
            remote.send(env.step(data))
        elif cmd == "reset":
            remote.send(env.reset(seed=data))
        elif cmd == "close":
            remote.close()
            break
        else:
            raise ValueError(cmd)


class SubprocVecEnv:
    def __init__(self, env_fns):
        # "fork" (Linux/Colab default) lets env_fns be closures; "spawn"
        # would require them to be importable top-level functions.
        ctx = mp.get_context("fork")
        self.remotes, work_remotes = zip(*[ctx.Pipe() for _ in env_fns])
        self.procs = [
            ctx.Process(target=_worker, args=(wr, fn), daemon=True)
            for wr, fn in zip(work_remotes, env_fns)
        ]
        for p in self.procs:
            p.start()

    @property
    def num_envs(self):
        return len(self.procs)

    def reset(self, seeds=None):
        seeds = seeds or [None] * self.num_envs
        for remote, s in zip(self.remotes, seeds):
            remote.send(("reset", s))
        return [remote.recv() for remote in self.remotes]

    def reset_one(self, idx, seed=None):
        self.remotes[idx].send(("reset", seed))
        return self.remotes[idx].recv()

    def step(self, actions_per_env):
        for remote, a in zip(self.remotes, actions_per_env):
            remote.send(("step", a))
        return [remote.recv() for remote in self.remotes]

    def close(self):
        for remote in self.remotes:
            remote.send(("close", None))
        for p in self.procs:
            p.join()
