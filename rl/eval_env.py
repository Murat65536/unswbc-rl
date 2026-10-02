"""Scores checkpoints natively, fast: the actor plays greedily (as the bot
does) in the batched environment against a scripted player, every game slot
at once, and this reports wins and how its queen fared.

    python -m rl.eval_env checkpoints/run6/ckpt_000260.pt checkpoints/run6/latest.pt --opponent careful

It uses the environment's own levels (generated maps, and the official ones
with --official-maps), so it is a quick way to compare checkpoints before
exporting one; export/evaluate.py is the real test, in the judge's sandbox.
"""
from __future__ import annotations

import argparse
from collections import Counter

import numpy as np
import torch

import bccore
from rl.policy import Actor, masked_logits
from rl.ppo import official_maps


def load(path: str) -> tuple[Actor, int]:
    from rl.warm_start import index_map, layout, widen_actor

    state = torch.load(path, map_location="cpu", weights_only=False)
    actor = Actor(bccore.OBS_SIZE, bccore.NUM_ACTIONS, bccore.feature_scales(), tuple(state["hidden"]))
    where = index_map(layout(state), (bccore.NUM_PLANES, bccore.NUM_SCALARS))
    actor.load_state_dict(widen_actor(state["actor"], where))
    actor.qat = bool(state.get("qat", False))
    actor.eval()
    return actor, int(state.get("mask_level", 1))


@torch.no_grad()
def score(actor: Actor, options: dict, games: int, slots: int, seed: int) -> dict:
    env = bccore.BatchEnv(slots, seed, options)
    obs = np.zeros((slots, bccore.OBS_SIZE), np.uint8)
    mask = np.zeros((slots, bccore.NUM_ACTIONS), np.uint8)
    priv = np.zeros((slots, bccore.NUM_PRIVILEGED), np.float32)
    prev_row = np.zeros(slots, np.int64)
    prev_reward = np.zeros(slots, np.float32)
    info = np.zeros((slots, bccore.NUM_INFO), np.int32)
    env.reset(obs, mask, priv, prev_row, prev_reward, info)
    # The first `quota` games of every slot, so long games count as much as
    # short ones, and every checkpoint plays the same levels.
    quota = -(-games // slots)
    done = [[] for _ in range(slots)]
    while any(len(d) < quota for d in done):
        logits = masked_logits(actor(torch.from_numpy(obs)), torch.from_numpy(mask))
        out = env.step(logits.argmax(1).to(torch.int32).numpy(), obs, mask, priv, prev_row, prev_reward, info)
        e = out["episodes"]
        for i in range(len(e["slot"])):
            slot = int(e["slot"][i])
            if len(done[slot]) < quota:
                done[slot].append({k: e[k][i].item() for k in e})
    episodes = [e for d in done for e in d]
    wins = draws = alive = splits = 0
    causes, deciders = Counter(), Counter()
    for e in episodes:
        learner = 1 - e["scripted_team"]
        side = "ab"[learner]
        wins += e["outcome"] == learner
        draws += e["outcome"] == 2
        alive += e[f"queen_died_{side}"] < 0
        splits += e[f"queen_splits_{side}"]
        causes[e[f"queen_death_{side}"]] += 1
        deciders[e["decider"]] += 1
    n = len(episodes)
    return {"games": n, "win": wins / n, "draw": draws / n, "queen_alive": alive / n, "queen_splits": splits / n,
            "rounds": float(np.mean([e["rounds"] for e in episodes])),
            "deaths": {name: causes[code] / n for code, name in
                       ((1, "cornered"), (2, "own move"), (3, "rammed"), (4, "rammed by ally"))},
            "decided": {name: deciders[code] / n for code, name in
                        ((0, "elimination"), (1, "queen"), (2, "longest"), (3, "total"), (4, "draw"))}}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("checkpoints", nargs="+")
    p.add_argument("--opponent", choices=("careful", "random"), default="careful")
    p.add_argument("--games", type=int, default=512, help="at least this many (a whole number per slot)")
    p.add_argument("--slots", type=int, default=256)
    p.add_argument("--official-maps", type=float, default=0.3)
    p.add_argument("--mask-level", type=int, default=None, help="default: the checkpoint's")
    p.add_argument("--seed", type=int, default=12345)
    p.add_argument("--threads", type=int, default=0)
    args = p.parse_args()

    for path in args.checkpoints:
        actor, level = load(path)
        options = {"scripted_frac": 1.0, "scripted_careful": 1.0 if args.opponent == "careful" else 0.0,
                   "mask_level": level if args.mask_level is None else args.mask_level, "threads": args.threads}
        if args.official_maps > 0:
            options["maps"] = official_maps()
            options["map_prob"] = args.official_maps
        r = score(actor, options, args.games, args.slots, args.seed)
        deaths = ", ".join(f"{k} {v:.2f}" for k, v in r["deaths"].items())
        decided = ", ".join(f"{k} {v:.2f}" for k, v in r["decided"].items() if v)
        print(f"{path} vs {args.opponent} (mask level {options['mask_level']}, {r['games']} games): "
              f"win {r['win']:.2f} draw {r['draw']:.2f} | queen alive {r['queen_alive']:.2f}, splits "
              f"{r['queen_splits']:.2f}, deaths: {deaths} | rounds {r['rounds']:.0f} | decided: {decided}", flush=True)


if __name__ == "__main__":
    main()
