"""The actor's deployed form: integers only.

From a trained Actor (rl/policy.py) this derives int8 weights, int32 biases
and fixed-point requantisation multipliers, using the very tensors and
rounding the quantisation-aware forward pass uses, and computes the integer
forward pass exactly as the C++ bot does (export/bot/net.h):

    a0 = observation codes (uint8)
    hidden layer k: acc = W_k a + b_k              (int32, exact)
                    a = clamp((acc * M_k + 2^31) >> 32, 0, 255)
    last layer:     logits = W a + b               (int32; one weight scale)
    action = the lowest-index argmax over the allowed actions

`IntegerActor.forward` is the reference the export gate holds the bot to.
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import torch

from rl.policy import Actor, quantize_weight


@dataclass
class IntegerLayer:
    weight: np.ndarray  # int8 [out, in]
    bias: np.ndarray    # int32 [out]
    multiplier: np.ndarray | None  # int64 [out] for hidden layers; None for the last


class IntegerActor:
    def __init__(self, layers: list[IntegerLayer]):
        self.layers = layers

    @property
    def sizes(self) -> list[int]:
        return [self.layers[0].weight.shape[1]] + [layer.weight.shape[0] for layer in self.layers]

    def logits(self, codes: np.ndarray) -> np.ndarray:
        """int64 logits for uint8 codes [B, obs]."""
        a = codes.astype(np.int64)
        for layer in self.layers:
            acc = a @ layer.weight.astype(np.int64).T + layer.bias.astype(np.int64)
            if layer.multiplier is None:
                return acc
            a = np.clip((acc * layer.multiplier + (1 << 31)) >> 32, 0, 255)
        raise AssertionError("unreachable")

    def act(self, codes: np.ndarray, mask: np.ndarray) -> np.ndarray:
        logits = self.logits(codes)
        logits = np.where(mask.astype(bool), logits, np.iinfo(np.int64).min)
        return logits.argmax(axis=1)  # the first maximum, as the bot does


@torch.no_grad()
def quantize(actor: Actor) -> IntegerActor:
    """The integer actor matching `actor`'s quantisation-aware forward pass
    (actor.qat should have been on while training; its act_max is frozen)."""
    layers = []
    last = len(actor.layers) - 1
    in_scale = None
    for i, layer in enumerate(actor.layers):
        w = layer.weight
        if i == 0:
            w = w * actor.scales
        codes, w_scale = quantize_weight(w, per_channel=i != last)
        acc_scale = w_scale.squeeze(1) * (1.0 if in_scale is None else in_scale)
        bias = torch.round(layer.bias / acc_scale)
        weight = codes.numpy().astype(np.int8)
        bias_np = bias.numpy().astype(np.int64)
        if np.abs(bias_np).max(initial=0) >= 2 ** 31:
            raise ValueError(f"layer {i}: a bias does not fit in int32")
        multiplier = None
        if i < last:
            out_scale = actor.act_max[i] / 255.0
            ratio = acc_scale.double() / out_scale.double()
            multiplier = torch.round(ratio * 2.0 ** 32).numpy().astype(np.int64)
            # The bot computes acc * M in int64: bound |acc| by the largest
            # input (64 for observation codes, 255 for activations).
            top = 64 if i == 0 else 255
            worst = np.abs(weight.astype(np.int64)).sum(axis=1) * top + np.abs(bias_np)
            if worst.max(initial=0) >= 2 ** 31:
                raise ValueError(f"layer {i}: the accumulator could overflow int32")
            if (worst.astype(np.float64) * multiplier.astype(np.float64)).max(initial=0) >= 2.0 ** 62:
                raise ValueError(f"layer {i}: requantisation could overflow int64")
            in_scale = out_scale
        layers.append(IntegerLayer(weight, bias_np.astype(np.int32), multiplier))
    return IntegerActor(layers)
