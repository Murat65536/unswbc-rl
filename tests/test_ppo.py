"""The trainer's bookkeeping (rl/ppo.py): rows complete exactly once, and the
vectorised GAE equals a plain per-dragon reference."""
import argparse
import pathlib
import sys

import numpy as np
import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

torch = pytest.importorskip("torch")
bccore = pytest.importorskip("bccore", reason="build the C++ core first (see README)")

from rl.train import parse_args  # noqa: E402
from rl.ppo import Trainer  # noqa: E402


def small_args(**overrides):
    args = parse_args(["--games", "6", "--rollout", "12", "--threads", "2", "--width", "10", "14",
                       "--height", "10", "14", "--league-frac", "0", "--plr-replay", "0", "--qat-after", "-1",
                       "--hidden", "32", "--critic-hidden", "32", "--max-rounds", "40"])
    for k, v in overrides.items():
        setattr(args, k, v)
    return args


def reference_gae(history, gamma, lam):
    """history: per rollout, (base, T, n, values[T+1, n], prev_row[T+1, n],
    prev_reward[T+1, n], done_row, done_reward, done_boot_value). Returns
    {row: adv} for every row completed in each rollout, the plain way."""
    value_of = {}
    out = []
    for base, T, n, values, prev_row, prev_reward, done_row, done_reward, done_boot in history:
        for t in range(T):
            for s in range(n):
                value_of[base + t * n + s] = float(values[t, s])
        completed = {}
        for t in range(1, T + 1):
            for s in range(n):
                p = int(prev_row[t, s])
                if p >= 0:
                    q = base + t * n + s
                    completed[p] = (float(prev_reward[t, s]), gamma * float(values[t, s]), q if t < T else None)
        for p, r, nv in zip(done_row, done_reward, done_boot):
            completed[int(p)] = (float(r), float(nv), None)
        adv = {}
        for p in sorted(completed, reverse=True):
            r, nv, q = completed[p]
            delta = r + nv - value_of[p]
            follow = adv[q] if q is not None and q in adv else 0.0
            adv[p] = delta + gamma * lam * follow
        out.append(adv)
    return out


def test_gae_matches_the_plain_reference_and_rows_train_once():
    args = small_args()
    trainer = Trainer(args)
    history, trained = [], []
    for _ in range(12):
        batch = trainer.collect()
        r = trainer.rollout
        values = torch.cat([r.value, batch["last_value"].unsqueeze(0)]).numpy().copy()
        history.append((batch["base"], args.rollout, args.games, values, r.prev_row.numpy().copy(),
                        r.prev_reward.numpy().copy(), batch["done_row"], batch["done_reward"],
                        batch["done_boot_value"]))
        data = trainer.advantages(batch)
        trained.append({int(row): float(a) for row, a in zip(data["row"], data["adv"])})
        trainer.rollout.roll()
    reference = reference_gae(history, args.gamma, args.gae_lambda)
    seen = set()
    for ours, theirs in zip(trained, reference):
        assert set(ours) == set(theirs)
        for row in ours:
            assert ours[row] == pytest.approx(theirs[row], abs=1e-4)
        assert not (seen & set(ours)), "a row was trained twice"
        seen |= set(ours)
    assert len(seen) > 500
    assert any(row < h[0] for h, ours in zip(history, trained) for row in ours), "carried rows were trained"
