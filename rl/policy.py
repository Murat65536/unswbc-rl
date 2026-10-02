"""The actor and the critic.

The actor sees only what a dragon sees: the shared core's uint8 observation
(core/features.h), feature i meaning code * scale[i]. It is a small MLP whose
deployed form is pure integer arithmetic, which the C++ bot runs with int8
weights and exact 32-bit accumulation (export/quantize.py, bot/net.h):

    layer 1: acc = W1q @ codes + b1q             (W1 folded with the scales)
    hidden:  a = clamp((acc * M + 2^31) >> 32, 0, 255)   per output channel
    layer k: acc = Wkq @ a + bkq
    logits:  acc of the last layer (one weight scale, so argmax is exact)

Training can run quantisation-aware (`qat`): weights and activations pass
through the same rounding on the forward pass, straight-through on the
backward pass, so the float actor and the integer one agree.

The critic is training-only and privileged: it also gets the standings of
both teams (core/state_view.h PrivilegedFeatures).
"""
from __future__ import annotations

import torch
import torch.nn as nn
import torch.nn.functional as F


def _round_ste(x: torch.Tensor) -> torch.Tensor:
    return x + (torch.round(x) - x).detach()


def quantize_weight(w: torch.Tensor, per_channel: bool) -> tuple[torch.Tensor, torch.Tensor]:
    """Symmetric int8: returns (codes in [-127, 127] as float, scale per row
    or one scale), straight-through for the rounding."""
    if per_channel:
        scale = w.detach().abs().amax(dim=1, keepdim=True).clamp_min(1e-8) / 127.0
    else:
        scale = w.detach().abs().amax().clamp_min(1e-8).reshape(1, 1) / 127.0
    codes = torch.clamp(_round_ste(w / scale), -127, 127)
    return codes, scale


class Actor(nn.Module):
    def __init__(self, obs_size: int, num_actions: int, scales, hidden=(256, 256)):
        super().__init__()
        self.register_buffer("scales", torch.as_tensor(scales, dtype=torch.float32))
        sizes = [obs_size, *hidden, num_actions]
        self.layers = nn.ModuleList(nn.Linear(a, b) for a, b in zip(sizes[:-1], sizes[1:]))
        # Running maximum of each hidden layer's activations: the uint8 scale
        # of that layer is act_max / 255.
        self.register_buffer("act_max", torch.ones(len(hidden)))
        self.qat = False
        # Whether a training-mode forward updates act_max (off for the
        # minibatch passes, which see the same data many times).
        self.track = True
        for layer in self.layers:
            nn.init.orthogonal_(layer.weight, gain=2 ** 0.5)
            nn.init.zeros_(layer.bias)
        nn.init.orthogonal_(self.layers[-1].weight, gain=0.01)

    @property
    def hidden(self):
        return [layer.out_features for layer in self.layers[:-1]]

    def forward(self, codes: torch.Tensor) -> torch.Tensor:
        """Logits for uint8 observation codes [B, obs]."""
        x = codes.float()
        last = len(self.layers) - 1
        in_scale = None  # the scale of x's codes; None: x is already real-valued
        for i, layer in enumerate(self.layers):
            w, b = layer.weight, layer.bias
            if i == 0:
                # Fold the feature scales into the first layer: x stays as raw codes.
                w = w * self.scales
            if self.qat:
                q, w_scale = quantize_weight(w, per_channel=i != last)
                acc_scale = w_scale.squeeze(1) * (1.0 if in_scale is None else in_scale)
                b = _round_ste(b / acc_scale) * acc_scale
                w = q * w_scale
            y = F.linear(x if in_scale is None else x * in_scale, w, b)
            if i == last:
                return y
            y = F.relu(y)
            if self.training and self.track:
                with torch.no_grad():
                    self.act_max[i] = 0.99 * self.act_max[i] + 0.01 * y.detach().amax().clamp_min(1e-3)
            if self.qat:
                scale = self.act_max[i] / 255.0
                x = torch.clamp(_round_ste(y / scale), 0, 255)
                in_scale = scale
            else:
                x, in_scale = y, None
        raise AssertionError("unreachable")


class Critic(nn.Module):
    def __init__(self, obs_size: int, privileged: int, scales, hidden=(256, 256)):
        super().__init__()
        self.register_buffer("scales", torch.as_tensor(scales, dtype=torch.float32))
        sizes = [obs_size + privileged, *hidden]
        layers = []
        for a, b in zip(sizes[:-1], sizes[1:]):
            layers += [nn.Linear(a, b), nn.ReLU()]
        self.trunk = nn.Sequential(*layers)
        self.head = nn.Linear(sizes[-1], 1)
        for m in self.modules():
            if isinstance(m, nn.Linear):
                nn.init.orthogonal_(m.weight, gain=2 ** 0.5)
                nn.init.zeros_(m.bias)
        nn.init.orthogonal_(self.head.weight, gain=1.0)

    def forward(self, codes: torch.Tensor, privileged: torch.Tensor) -> torch.Tensor:
        x = torch.cat([codes.float() * self.scales, privileged], dim=1)
        return self.head(self.trunk(x)).squeeze(-1)


def masked_logits(logits: torch.Tensor, mask: torch.Tensor) -> torch.Tensor:
    return logits.masked_fill(~mask.bool(), torch.finfo(logits.dtype).min)
