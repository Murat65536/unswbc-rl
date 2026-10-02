"""Self-play PPO on the C++ environment.

    python -m rl.train --games 1024 --rollout 64 --iterations 2000 \\
        --checkpoint-dir checkpoints --log-csv checkpoints/log.csv

Every living dragon on both teams, in every game, is an agent of one shared
actor (the same program runs every dragon in the real game too). The critic
is privileged and training-only. See rl/ppo.py for how rollouts, rows and
completions fit together, and core/batch_env.h for the environment.
"""
from __future__ import annotations

import argparse
import csv
import os
import pathlib
import time

import numpy as np
import torch

from .ppo import Trainer


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    env = p.add_argument_group("environment")
    env.add_argument("--games", type=int, default=1024, help="game slots stepped together")
    env.add_argument("--threads", type=int, default=0, help="environment threads (0: one per core)")
    env.add_argument("--mask-level", type=int, default=1, choices=(0, 1))
    env.add_argument("--max-rounds", type=int, default=500, help="training horizon (500 plays every game out)")
    env.add_argument("--shaping", type=float, default=0.5, help="scale of the potential-based shaping")
    env.add_argument("--queen-weight", type=float, default=0.5)
    env.add_argument("--width", type=int, nargs=2, default=(16, 40))
    env.add_argument("--height", type=int, nargs=2, default=(16, 40))
    env.add_argument("--dragons-per-team", type=int, nargs=2, default=(1, 4))
    env.add_argument("--kelp", type=float, nargs=2, default=(0.0, 0.12))
    env.add_argument("--portal-pairs", type=int, nargs=2, default=(0, 4))
    env.add_argument("--official-maps", type=float, default=0.0,
                     help="probability a level is one of the 15 official maps (needs unswbc installed)")

    ppo = p.add_argument_group("PPO")
    ppo.add_argument("--iterations", type=int, default=1000)
    ppo.add_argument("--rollout", type=int, default=64, help="environment steps per iteration")
    ppo.add_argument("--epochs", type=int, default=3)
    ppo.add_argument("--minibatch", type=int, default=8192)
    ppo.add_argument("--lr", type=float, default=3e-4)
    ppo.add_argument("--gamma", type=float, default=0.995)
    ppo.add_argument("--gae-lambda", type=float, default=0.95)
    ppo.add_argument("--clip", type=float, default=0.2)
    ppo.add_argument("--value-clip", type=float, default=10.0)
    ppo.add_argument("--ent-coef", type=float, default=0.01)
    ppo.add_argument("--vf-coef", type=float, default=0.5)
    ppo.add_argument("--max-grad-norm", type=float, default=0.5)
    ppo.add_argument("--hidden", type=int, nargs="+", default=(256, 256), help="actor hidden sizes")
    ppo.add_argument("--critic-hidden", type=int, nargs="+", default=(256, 256))
    ppo.add_argument("--qat-after", type=int, default=0,
                     help="train quantisation-aware from this iteration on (-1: never)")

    league = p.add_argument_group("league and level replay")
    league.add_argument("--league-frac", type=float, default=0.25,
                        help="share of game slots where the learner plays a frozen earlier snapshot")
    league.add_argument("--league-size", type=int, default=10)
    league.add_argument("--snapshot-every", type=int, default=25)
    league.add_argument("--opponent-every", type=int, default=5)
    league.add_argument("--scripted-frac", type=float, default=0.0,
                        help="share of game slots (the last ones) where one team is a scripted random-safe player")
    league.add_argument("--plr-replay", type=float, default=0.5, help="chance a new level is a replayed one (0: off)")
    league.add_argument("--plr-capacity", type=int, default=4000)
    league.add_argument("--plr-temperature", type=float, default=0.3)
    league.add_argument("--plr-staleness", type=float, default=0.1)

    run = p.add_argument_group("run")
    run.add_argument("--seed", type=int, default=0)
    run.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    run.add_argument("--torch-threads", type=int, default=0)
    run.add_argument("--checkpoint-dir", default="checkpoints")
    run.add_argument("--checkpoint-every", type=int, default=20)
    run.add_argument("--resume", default=None, help="a checkpoint to continue from")
    run.add_argument("--log-csv", default=None)
    return p.parse_args(argv)


def save_atomic(state: dict, path: pathlib.Path):
    """Write to a temporary file and rename over the target, so a crash
    mid-write never leaves a truncated checkpoint behind."""
    tmp = path.with_suffix(path.suffix + ".tmp")
    torch.save(state, tmp)
    os.replace(tmp, path)


def main(argv=None):
    args = parse_args(argv)
    if args.torch_threads:
        torch.set_num_threads(args.torch_threads)
    out_dir = pathlib.Path(args.checkpoint_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    trainer = Trainer(args, device=args.device)
    if args.resume:
        trainer.load_state_dict(torch.load(args.resume, map_location="cpu", weights_only=False))
        print(f"resumed from {args.resume} at iteration {trainer.iteration}")

    writer = None
    log_file = None
    columns = ["iteration", "decisions", "trained_rows", "episodes", "mean_rounds", "mean_total_length", "draw_rate",
               "league_win_rate", "scripted_win_rate", "policy_loss", "value_loss", "entropy", "approx_kl", "clip_frac",
               "decisions_per_second", "env_seconds", "seconds", "carried", "plr_levels"]
    if args.log_csv:
        new = not os.path.exists(args.log_csv)
        log_file = open(args.log_csv, "a", newline="")
        writer = csv.DictWriter(log_file, fieldnames=columns, extrasaction="ignore")
        if new:
            writer.writeheader()

    start = time.time()
    while trainer.iteration < args.iterations:
        stats = trainer.iterate()
        line = (f"iter {stats['iteration']:5d} | dec {stats['decisions']:11,d} | "
                f"{stats['decisions_per_second'] / 1e3:6.1f}k dec/s (env {stats['env_seconds']:.1f}s of "
                f"{stats['seconds']:.1f}s) | games {stats['episodes']:4d} rounds {stats.get('mean_rounds', 0):5.1f} "
                f"len {stats.get('mean_total_length', 0):5.1f}")
        if "league_win_rate" in stats:
            line += f" | league win {stats['league_win_rate']:.2f} ({stats['league_games']})"
        if "scripted_win_rate" in stats:
            line += f" | vs scripted {stats['scripted_win_rate']:.2f} ({stats['scripted_games']})"
        if "entropy" in stats:
            line += f" | H {stats['entropy']:.3f} kl {stats['approx_kl']:.4f} vL {stats['value_loss']:.4f}"
        print(line, flush=True)
        if writer:
            writer.writerow({k: stats.get(k, "") for k in columns})
            log_file.flush()
        if trainer.iteration % args.checkpoint_every == 0 or trainer.iteration == args.iterations:
            state = trainer.state_dict()
            save_atomic(state, out_dir / f"ckpt_{trainer.iteration:06d}.pt")
            save_atomic(state, out_dir / "latest.pt")
    if log_file:
        log_file.close()
    print(f"done in {(time.time() - start) / 60:.1f} min")


if __name__ == "__main__":
    main()
