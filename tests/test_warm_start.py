"""Warm starts across observation layouts (rl/warm_start.py): an actor or
critic widened to the current observation computes exactly what it did on the
old one, in float and quantisation-aware, and a trainer starts from it."""
import pathlib
import sys

import numpy as np
import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

torch = pytest.importorskip("torch")
bccore = pytest.importorskip("bccore", reason="build the C++ core first (see README)")

from rl.policy import Actor, Critic  # noqa: E402
from rl.ppo import Trainer  # noqa: E402
from rl.warm_start import index_map, init_from, widen_actor, widen_critic  # noqa: E402
from test_ppo import small_args  # noqa: E402

OLD = (22, 38)  # bots/rl_bot_v2's layout
OLD_ACTIONS = 13  # before shed and dissolve
WHERE = index_map(OLD, (bccore.NUM_PLANES, bccore.NUM_SCALARS))


def old_scales():
    return bccore.feature_scales()[WHERE]


def random_codes(rows, seed=0):
    rng = np.random.default_rng(seed)
    scales = bccore.feature_scales()
    top = np.where(scales < 1, np.round(1 / scales), 1).astype(np.int64)
    return torch.from_numpy((rng.integers(0, 1 << 30, (rows, bccore.OBS_SIZE)) % (top + 1)).astype(np.uint8))


def test_the_old_features_keep_their_meaning():
    assert len(WHERE) == OLD[0] * 49 + OLD[1]
    assert np.array_equal(old_scales(), bccore.feature_scales()[WHERE])
    assert WHERE[-1] < bccore.OBS_SIZE and len(set(WHERE)) == len(WHERE)


@pytest.mark.parametrize("qat", [False, True])
def test_a_widened_actor_acts_as_before(qat):
    torch.manual_seed(0)
    old = Actor(len(WHERE), OLD_ACTIONS, old_scales(), (64, 32))
    old.act_max.fill_(4.0)
    new = Actor(bccore.OBS_SIZE, bccore.NUM_ACTIONS, bccore.feature_scales(), (64, 32))
    new.load_state_dict(widen_actor(old.state_dict(), WHERE))
    old.qat = new.qat = qat
    old.eval()
    new.eval()
    codes = random_codes(256)
    with torch.no_grad():
        ours, theirs = new(codes), old(codes[:, WHERE])
        assert torch.allclose(ours[:, :OLD_ACTIONS], theirs, atol=1e-5)
        if not qat:  # new actions start just below the split, whatever the input
            start = old.layers[-1].bias[12] - 2.0
            assert torch.allclose(ours[:, OLD_ACTIONS:], start.expand(256, bccore.NUM_ACTIONS - OLD_ACTIONS))


def test_a_widened_critic_values_as_before():
    torch.manual_seed(1)
    old = Critic(len(WHERE), bccore.NUM_PRIVILEGED, old_scales(), (64,))
    new = Critic(bccore.OBS_SIZE, bccore.NUM_PRIVILEGED, bccore.feature_scales(), (64,))
    new.load_state_dict(widen_critic(old.state_dict(), WHERE))
    codes = random_codes(64, seed=2)
    priv = torch.rand(64, bccore.NUM_PRIVILEGED)
    with torch.no_grad():
        assert torch.allclose(new(codes, priv), old(codes[:, WHERE], priv), atol=1e-5)


def test_a_trainer_starts_from_an_older_checkpoint():
    args = small_args()
    torch.manual_seed(3)
    old = Actor(len(WHERE), OLD_ACTIONS, old_scales(), tuple(args.hidden))
    critic = Critic(len(WHERE), bccore.NUM_PRIVILEGED, old_scales(), tuple(args.critic_hidden))
    state = {"actor": old.state_dict(), "critic": critic.state_dict(), "hidden": list(args.hidden),
             "obs_size": len(WHERE), "qat": False, "snapshots": [old.state_dict()]}
    trainer = Trainer(args)
    init_from(trainer, state)
    codes = random_codes(32, seed=4)
    with torch.no_grad():
        trainer.actor.eval()
        old.eval()
        assert torch.allclose(trainer.actor(codes)[:, :OLD_ACTIONS], old(codes[:, WHERE]), atol=1e-5)
    assert trainer.snapshots[0]["layers.0.weight"].shape[1] == bccore.OBS_SIZE
    stats = trainer.iterate()
    assert stats["iteration"] == 1
