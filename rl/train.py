"""Self-play PPO training entrypoint.

Example (local smoke test, tiny and fast):
    python -m rl.train --num-envs 8 --team-size 2 --width 14 --height 14 \
        --max-rounds 150 --iterations 20 --rollout-length 64

On Colab, point --checkpoint-dir at a Google Drive path so checkpoints
survive runtime restarts; see notebooks/train_colab.ipynb.
"""
from __future__ import annotations

import argparse
import csv
import os
import time

import numpy as np
import torch

from sim.env import BattlecodeMultiAgentEnv
from .policy import ActorCritic
from .ppo import PPOTrainer
from .vector_env import SerialVecEnv, SubprocVecEnv


def make_env_fn(args):
    def _fn():
        return BattlecodeMultiAgentEnv(
            width=args.width,
            height=args.height,
            team_size=args.team_size,
            max_rounds=args.max_rounds,
            kelp_prob=args.kelp_prob,
            portal_pairs=args.portal_pairs,
        )
    return _fn


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--num-envs", type=int, default=16)
    p.add_argument("--subproc", action="store_true", help="run envs in separate processes")
    p.add_argument("--width", type=int, default=16)
    p.add_argument("--height", type=int, default=16)
    p.add_argument("--team-size", type=int, default=2)
    p.add_argument("--max-rounds", type=int, default=250)
    p.add_argument("--kelp-prob", type=float, default=0.04)
    p.add_argument("--portal-pairs", type=int, default=0)

    p.add_argument("--iterations", type=int, default=1000)
    p.add_argument("--rollout-length", type=int, default=128)
    p.add_argument("--epochs", type=int, default=4)
    p.add_argument("--minibatch-size", type=int, default=4096)
    p.add_argument("--lr", type=float, default=3e-4)
    p.add_argument("--gamma", type=float, default=0.99)
    p.add_argument("--gae-lambda", type=float, default=0.95)
    p.add_argument("--clip", type=float, default=0.2)
    p.add_argument("--ent-coef", type=float, default=0.01)
    p.add_argument("--vf-coef", type=float, default=0.5)
    p.add_argument("--hidden", type=int, default=256)

    p.add_argument("--seed", type=int, default=0)
    p.add_argument("--device", type=str, default="cuda" if torch.cuda.is_available() else "cpu")
    p.add_argument("--checkpoint-dir", type=str, default="checkpoints")
    p.add_argument("--checkpoint-every", type=int, default=20)
    p.add_argument("--resume", type=str, default=None, help="path to a checkpoint .pt to resume from")
    p.add_argument("--log-csv", type=str, default=None)
    return p.parse_args()


def main():
    args = parse_args()
    torch.manual_seed(args.seed)
    np.random.seed(args.seed)
    os.makedirs(args.checkpoint_dir, exist_ok=True)

    vec_cls = SubprocVecEnv if args.subproc else SerialVecEnv
    vec_env = vec_cls([make_env_fn(args) for _ in range(args.num_envs)])

    policy = ActorCritic(hidden=args.hidden)
    start_iter = 0
    if args.resume:
        ckpt = torch.load(args.resume, map_location="cpu")
        policy.load_state_dict(ckpt["model"])
        start_iter = ckpt.get("iteration", 0)
        print(f"resumed from {args.resume} at iteration {start_iter}")

    trainer = PPOTrainer(
        vec_env, policy, device=args.device, gamma=args.gamma, gae_lambda=args.gae_lambda,
        lr=args.lr, clip=args.clip, vf_coef=args.vf_coef, ent_coef=args.ent_coef,
        epochs=args.epochs, minibatch_size=args.minibatch_size, seed=args.seed,
    )
    if args.resume:
        trainer.optimizer.load_state_dict(ckpt["optimizer"])

    csv_writer = None
    csv_file = None
    if args.log_csv:
        new_file = not os.path.exists(args.log_csv)
        csv_file = open(args.log_csv, "a", newline="")
        csv_writer = csv.writer(csv_file)
        if new_file:
            csv_writer.writerow([
                "iteration", "total_steps", "mean_episode_reward", "mean_episode_length",
                "mean_final_length", "win_rate_a", "win_rate_b", "draw_rate",
                "policy_loss", "value_loss", "entropy", "seconds",
            ])

    for it in range(start_iter, args.iterations):
        t0 = time.time()
        trajectories, rollout_stats = trainer.collect_rollout(args.rollout_length)
        update_stats = trainer.update(trajectories)
        dt = time.time() - t0

        winners = rollout_stats.get("game_winner", [])
        n = max(len(winners), 1)
        win_a = winners.count("A") / n
        win_b = winners.count("B") / n
        draw = winners.count("draw") / n
        mean_reward = float(np.mean(rollout_stats.get("episode_reward", [0.0])))
        mean_len = float(np.mean(rollout_stats.get("episode_length", [0.0])))
        mean_final_len = float(np.mean(rollout_stats.get("final_dragon_length", [0.0])))
        pol_loss = float(np.mean(update_stats["policy_loss"]))
        val_loss = float(np.mean(update_stats["value_loss"]))
        entropy = float(np.mean(update_stats["entropy"]))

        print(
            f"iter {it:5d} | steps {trainer.total_steps:9d} | "
            f"games {len(winners):3d} (A {win_a:.2f} B {win_b:.2f} draw {draw:.2f}) | "
            f"R {mean_reward:+6.2f} | len {mean_len:6.1f} | final_len {mean_final_len:5.2f} | "
            f"pLoss {pol_loss:+.4f} vLoss {val_loss:.4f} H {entropy:.3f} | {dt:.1f}s"
        )
        if csv_writer:
            csv_writer.writerow([it, trainer.total_steps, mean_reward, mean_len, mean_final_len,
                                  win_a, win_b, draw, pol_loss, val_loss, entropy, dt])
            csv_file.flush()

        if (it + 1) % args.checkpoint_every == 0 or it == args.iterations - 1:
            path = os.path.join(args.checkpoint_dir, f"ckpt_{it + 1:06d}.pt")
            torch.save({
                "model": policy.state_dict(),
                "optimizer": trainer.optimizer.state_dict(),
                "iteration": it + 1,
                "args": vars(args),
            }, path)
            latest = os.path.join(args.checkpoint_dir, "latest.pt")
            torch.save({
                "model": policy.state_dict(),
                "optimizer": trainer.optimizer.state_dict(),
                "iteration": it + 1,
                "args": vars(args),
            }, latest)
            print(f"  saved checkpoint -> {path}")

    vec_env.close()
    if csv_file:
        csv_file.close()


if __name__ == "__main__":
    main()
