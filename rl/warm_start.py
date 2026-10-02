"""Starting a run from a checkpoint made for an older observation layout, or
an older action set.

core/features.h only ever appends features: new planes after the planes, new
scalars after the scalars. So an older actor or critic is widened by giving
each new input zero weight: it computes exactly what it did before (the
integer actor too, since a zero column changes no row's quantisation scale),
and training takes the new inputs up from there. New actions are appended
too; each gets a zero row and a bias below the old split's, so the policy
starts out taking them rarely and learns when they pay.

    python -m rl.train --init-from checkpoints/run3/ckpt_000540.pt ...
"""
from __future__ import annotations

import numpy as np
import torch

import bccore

TILES = 49
# Layouts from before checkpoints recorded theirs, by observation size.
KNOWN_LAYOUTS = {1116: (22, 38)}  # bots/rl_bot_v2


def layout(state: dict) -> tuple[int, int]:
    """(planes, scalars) of the observation a checkpoint was trained on."""
    if "obs_planes" in state:
        return int(state["obs_planes"]), int(state["obs_scalars"])
    size = int(state["obs_size"])
    if size not in KNOWN_LAYOUTS:
        raise ValueError(f"no known observation layout of size {size}")
    return KNOWN_LAYOUTS[size]


def index_map(old: tuple[int, int], new: tuple[int, int]) -> np.ndarray:
    """Where each feature of the old layout sits in the new one."""
    (old_planes, old_scalars), (new_planes, new_scalars) = old, new
    if new_planes < old_planes or new_scalars < old_scalars:
        raise ValueError(f"layout {new} does not extend {old}")
    return np.concatenate([np.arange(old_planes * TILES), new_planes * TILES + np.arange(old_scalars)])


def _widen(weight: torch.Tensor, where: np.ndarray, width: int) -> torch.Tensor:
    """weight [out, len(where) + extra] -> [out, width + extra], column i of
    the first part moved to where[i], the extra columns kept at the end."""
    extra = weight.shape[1] - len(where)
    out = weight.new_zeros(weight.shape[0], width + extra)
    out[:, torch.as_tensor(where)] = weight[:, :len(where)]
    out[:, width:] = weight[:, len(where):]
    return out


# The split action, whose bias new actions start from (core/features.h).
SPLIT_ACTION = 12
NEW_ACTION_HANDICAP = 2.0


def widen_actor(state_dict: dict, where: np.ndarray) -> dict:
    out = dict(state_dict)
    out["layers.0.weight"] = _widen(state_dict["layers.0.weight"], where, bccore.OBS_SIZE)
    out["scales"] = torch.as_tensor(bccore.feature_scales(), dtype=torch.float32)
    last = max(int(k.split(".")[1]) for k in state_dict if k.startswith("layers.") and k.endswith(".weight"))
    weight, bias = state_dict[f"layers.{last}.weight"], state_dict[f"layers.{last}.bias"]
    extra = bccore.NUM_ACTIONS - weight.shape[0]
    if extra > 0:
        out[f"layers.{last}.weight"] = torch.cat([weight, weight.new_zeros(extra, weight.shape[1])])
        start = bias[SPLIT_ACTION] - NEW_ACTION_HANDICAP
        out[f"layers.{last}.bias"] = torch.cat([bias, start.repeat(extra)])
    return out


def widen_critic(state_dict: dict, where: np.ndarray) -> dict:
    out = dict(state_dict)
    out["trunk.0.weight"] = _widen(state_dict["trunk.0.weight"], where, bccore.OBS_SIZE)
    out["scales"] = torch.as_tensor(bccore.feature_scales(), dtype=torch.float32)
    return out


def init_from(trainer, state: dict):
    """Loads a checkpoint's actor, critic and league snapshots into a fresh
    trainer, widened to the current observation. The optimizer, the iteration
    count and level replay start afresh."""
    where = index_map(layout(state), (bccore.NUM_PLANES, bccore.NUM_SCALARS))
    if list(state["hidden"]) != list(trainer.actor.hidden):
        raise ValueError(f"the checkpoint's actor has hidden sizes {state['hidden']}, not {trainer.actor.hidden}")
    trainer.actor.load_state_dict(widen_actor(state["actor"], where))
    trainer.critic.load_state_dict(widen_critic(state["critic"], where))
    trainer.actor.qat = bool(state.get("qat", False))
    trainer.snapshots = [widen_actor(s, where) for s in state.get("snapshots", [])]
