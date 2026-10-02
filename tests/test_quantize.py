"""The integer actor (export/quantize.py) agrees with the quantisation-aware
float actor it was derived from."""
import pathlib
import sys

import numpy as np
import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

torch = pytest.importorskip("torch")
bccore = pytest.importorskip("bccore", reason="build the C++ core first (see README)")

from export.quantize import quantize  # noqa: E402
from rl.policy import Actor, masked_logits  # noqa: E402


def observations(n=4096, seed=0):
    env = bccore.BatchEnv(256, seed, {"threads": 2})
    obs = np.zeros((256, bccore.OBS_SIZE), np.uint8)
    mask = np.zeros((256, bccore.NUM_ACTIONS), np.uint8)
    priv = np.zeros((256, bccore.NUM_PRIVILEGED), np.float32)
    prev_row = np.zeros(256, np.int64)
    prev_reward = np.zeros(256, np.float32)
    info = np.zeros((256, bccore.NUM_INFO), np.int32)
    env.reset(obs, mask, priv, prev_row, prev_reward, info)
    rng = np.random.default_rng(seed)
    all_obs, all_mask = [], []
    while sum(len(o) for o in all_obs) < n:
        all_obs.append(obs.copy())
        all_mask.append(mask.copy())
        actions = np.argmax(rng.random(mask.shape) * mask, axis=1).astype(np.int32)
        env.step(actions, obs, mask, priv, prev_row, prev_reward, info)
    return np.concatenate(all_obs)[:n], np.concatenate(all_mask)[:n]


@pytest.mark.parametrize("hidden", [(64,), (128, 64)])
def test_integer_actor_matches_the_qat_actor(hidden):
    torch.manual_seed(0)
    obs, mask = observations()
    actor = Actor(bccore.OBS_SIZE, bccore.NUM_ACTIONS, bccore.feature_scales(), hidden)
    with torch.no_grad():
        for layer in actor.layers:
            layer.weight.normal_(0, 0.3 / layer.in_features ** 0.5 * 4)
            layer.bias.normal_(0, 0.1)
    actor.qat = True
    actor.train()
    with torch.no_grad():
        for _ in range(300):  # settle the activation ranges
            actor(torch.from_numpy(obs[:1024]))
    actor.eval()
    integer = quantize(actor)

    with torch.no_grad():
        float_logits = masked_logits(actor(torch.from_numpy(obs)), torch.from_numpy(mask)).numpy()
    ours = integer.act(obs, mask)
    theirs = float_logits.argmax(axis=1)
    agree = (ours == theirs).mean()
    assert agree > 0.995, agree
    # The integer logits are the float ones in units of the last layer's scale.
    int_logits = integer.logits(obs).astype(np.float64)
    corr = np.corrcoef(int_logits[mask.astype(bool)], float_logits[mask.astype(bool)])[0, 1]
    assert corr > 0.9999
