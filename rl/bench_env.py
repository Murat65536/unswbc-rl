"""Measures the C++ training environment's throughput, with no policy:
actions are drawn uniformly from each decision's allowed set.

    python -m rl.bench_env --games 4096 --threads 4 --steps 300
"""
from __future__ import annotations

import argparse
import time

import numpy as np

import bccore


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--games", type=int, nargs="+", default=[256, 1024, 4096])
    p.add_argument("--threads", type=int, nargs="+", default=[1, 0])
    p.add_argument("--steps", type=int, default=300)
    args = p.parse_args()

    rng = np.random.default_rng(0)
    for n in args.games:
        for threads in args.threads:
            env = bccore.BatchEnv(n, 0, {"threads": threads})
            obs = np.zeros((n, bccore.OBS_SIZE), np.uint8)
            mask = np.zeros((n, bccore.NUM_ACTIONS), np.uint8)
            priv = np.zeros((n, bccore.NUM_PRIVILEGED), np.float32)
            prev_row = np.zeros(n, np.int64)
            prev_reward = np.zeros(n, np.float32)
            info = np.zeros((n, bccore.NUM_INFO), np.int32)
            env.reset(obs, mask, priv, prev_row, prev_reward, info)
            spent = 0.0
            for _ in range(args.steps):
                actions = np.argmax(rng.random(mask.shape, dtype=np.float32) * mask, axis=1).astype(np.int32)
                start = time.perf_counter()
                env.step(actions, obs, mask, priv, prev_row, prev_reward, info)
                spent += time.perf_counter() - start
            rate = n * args.steps / spent
            print(f"games {n:6d}  threads {env.threads:3d}  {rate / 1e6:6.2f}M decisions/s (environment only)")


if __name__ == "__main__":
    main()
