"""Shared actor-critic policy, evaluated once per living dragon per turn.

Every dragon on both teams runs the *same* weights -- there is no
inter-dragon shared memory in the real game (each dragon is a separate
process), and parameter sharing across fully symmetric, interchangeable
agents is the standard self-play setup for games like this. The exported
competition bot (export/) re-implements this network's forward pass in
plain NumPy, since that's all the judge's sandbox gives a Python bot.
"""
from __future__ import annotations

import torch
import torch.nn as nn

from sim.obs import OBS_SIZE


class ActorCritic(nn.Module):
    def __init__(self, obs_size: int = OBS_SIZE, num_actions: int = 4, hidden: int = 256):
        super().__init__()
        self.trunk = nn.Sequential(
            nn.Linear(obs_size, hidden),
            nn.ReLU(),
            nn.Linear(hidden, hidden),
            nn.ReLU(),
        )
        self.actor = nn.Linear(hidden, num_actions)
        self.critic = nn.Linear(hidden, 1)

        for m in self.modules():
            if isinstance(m, nn.Linear):
                nn.init.orthogonal_(m.weight, gain=1.414)
                nn.init.zeros_(m.bias)
        nn.init.orthogonal_(self.actor.weight, gain=0.01)
        nn.init.orthogonal_(self.critic.weight, gain=1.0)

    def forward(self, obs: torch.Tensor):
        h = self.trunk(obs)
        return self.actor(h), self.critic(h).squeeze(-1)

    def export_weights(self):
        """Plain {name: numpy array} dict for export/export_bot.py."""
        return {name: p.detach().cpu().numpy() for name, p in self.state_dict().items()}
